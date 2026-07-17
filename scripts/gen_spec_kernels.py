#!/usr/bin/env python3
"""
Generate one self-contained PolyBench-shaped kernel per SPEC CPU2017 (C-only)
benchmark, so the existing COMET harness (optimize.py / tune_param.py /
tune_source.py) runs against it completely unmodified -- same design as
scripts/gen_tsvc_kernels.py and scripts/gen_cbench_kernels.py.

Per benchmark (hand-described in BENCHMARKS below, since unlike cBench's CK
meta.json, SPEC's Spec/object.pm invoke() is bespoke Perl per benchmark --
no uniform schema to auto-parse):
  - rename the real entry point (`main`) to `kernel_<name>`
  - flatten every header used by the benchmark (recursive search under
    src/, skipping platform dirs like win32/) into one directory, copied
    into BOTH kernels/<name>/ (source_dir, for the driver TU) and
    utilities/ (for the amalgamated polybench.c TU) -- this is safe here
    because within a single SPEC benchmark's source tree, header basenames
    are unique (verified per-benchmark before generation).
  - all OTHER .c files (not the entry file) get concatenated, in source
    order, after the real polybench.c into a benchmark-private polybench.c
    copy -- a unity build of exactly the non-entry files, same trick
    gen_cbench_kernels.py uses for multi-file cBench programs.
  - SPEC's own harness always compiles with -DSPEC -DNDEBUG (see
    benchspec/Makefile.defaults: CPUFLAGS = -DSPEC -DNDEBUG); we bake these
    into a tiny spec_defines.h that is #include'd first.
  - the driver is called directly with a hardcoded argv built from the
    benchmark's "test" workload (data/test/input/...), resolved to
    absolute paths so no chdir is required. Args below were read directly
    out of each benchmark's Spec/object.pm invoke() sub and its
    data/test/input/* files.
  - no polybench.h, no timing/dump macros, no stdout redirect: the wrapper
    just calls the renamed entry point and returns. Timing is external
    wall-clock (src/build_utils.py::run_timing) and correctness is checked
    against the program's own real stdout (src/correctness.py numeric/hash
    tiers) -- see docs/GENERIC_HARNESS_DESIGN.md for why.

Each generated kernel is actually test-compiled with clang-11 and only
kept in the manifest if it succeeds.

Candidates covered so far (pure C, single `main`, small-to-medium file
count -- see docs/SPEC2017_STATUS.md for the full 43-benchmark survey and
why the rest are deferred):
    505.mcf_r   (11 files, network simplex solver)
    519.lbm_r   ( 2 files, lattice-Boltzmann fluid dynamics)
Not yet done (documented, not attempted here):
    544.nab_r    -- needs argv[1] resolved relative to a data subdir
                    (chdir semantics), not just flat absolute paths.
    557.xz_r     -- two `main()`s (spec.c harness + pxz.c), ~70 files.
    538.imagick_r, 500.perlbench_r, 502.gcc_r -- SPEC harness
                    (spec.c/SpecMain) dispatch pattern, large file counts.
    Fortran-language and CXX-language benchmarks -- out of scope for this
    C-only, clang -mllvm-flag-driven harness.
"""
import re
import shutil
import subprocess
from pathlib import Path

SPEC_ROOT = Path("/home/hanning/spec2017/benchspec/CPU")
DATA_ROOT = Path("/home/hanning/spec2017/benchspec/CPU")
POLYBENCH_C = Path("/home/hanning/comet/PolyBenchC_no_rag/utilities/polybench.c")
POLYBENCH_H = Path("/home/hanning/comet/PolyBenchC_no_rag/utilities/polybench.h")
OUT_ROOT = Path("/home/hanning/comet/SPEC_shim_root")
TMP_DIR = Path("/home/hanning/comet/tmp")
CLANG = "/usr/bin/clang-11"

SPEC_DEFINES_H = "#ifndef SPEC\n#define SPEC 1\n#endif\n#ifndef NDEBUG\n#define NDEBUG 1\n#endif\n"

BENCHMARKS = {
    "mcf_r": {
        "bench_dir": "505.mcf_r",
        "entry_file": "mcf.c",
        "sources": [
            "mcf.c", "mcfutil.c", "readmin.c", "implicit.c", "pstart.c",
            "output.c", "treeup.c", "pbla.c", "pflowup.c", "psimplex.c",
            "pbeampp.c", "spec_qsort/spec_qsort.c",
        ],
        "skip_dirs": {"win32"},
        # argv built from data/test/input/control -> "inp.in" (name has no
        # digit suffix so mcf.c's Spec/object.pm invoke() outnum stays "").
        "argv": lambda bdir: [
            "kernel_mcf_r",
            str(bdir / "data/test/input/inp.in"),
            "",
        ],
    },
    "lbm_r": {
        "bench_dir": "519.lbm_r",
        "entry_file": "main.c",
        "sources": ["lbm.c", "main.c"],
        "skip_dirs": set(),
        # data/test/input/lbm.in: "20 reference.dat 0 1 100_100_130_cf_a.of"
        # (timesteps, result file [output, abs-pathed into tmp/], action,
        # simType, obstacle file [input, abs-pathed]).
        "argv": lambda bdir: [
            "kernel_lbm_r",
            "20",
            str(TMP_DIR / "lbm_r_reference.dat"),
            "0",
            "1",
            str(bdir / "data/test/input/100_100_130_cf_a.of"),
        ],
    },
}


def rename_entry_all(text: str, entry: str, new_name: str) -> str:
    """Rename every occurrence of the bare identifier `entry` immediately
    followed by '(' (i.e. every call/definition token, not substrings like
    'domain(') to new_name. mcf.c has TWO textual definitions of main()
    guarded by #ifdef _PROTO_ / #else (ANSI vs K&R) with different parameter
    lists -- renaming by parameter shape doesn't generalize across
    benchmarks' varying signatures (lbm's main() uses `int nArgs` instead of
    `int argc`), so we key only on the identifier + '(' boundary instead."""
    pattern = r"(?<![A-Za-z_0-9])" + re.escape(entry) + r"(\s*\()"
    matches = list(re.finditer(pattern, text))
    assert matches, f"no occurrences of {entry}( found"
    out, last = [], 0
    for m in matches:
        out.append(text[last:m.start()])
        out.append(new_name)
        out.append(m.group(1))
        last = m.end()
    out.append(text[last:])
    return "".join(out)


# Generic wrapper -- no polybench.h, no dump macros, no stdout redirect.
# Timing is done externally (wall-clock around the whole process, see
# src/build_utils.py::run_timing), and correctness is checked against the
# program's OWN real stdout (see src/correctness.py) instead of a
# manufactured checksum. Earlier this redirected the kernel's real stdout
# to a fixed per-kernel-name tmp file and only put a checksum on the
# process's actual stdout -- besides needing polybench.h just for that
# checksum's dump macros, that fixed path is a race: two runs of the same
# kernel (reference vs. candidate, or a future concurrent evaluator) can
# clobber each other's file mid-read. Piping the kernel's real output
# straight through the process's own stdout is race-free by construction
# -- each subprocess gets its own private pipe -- and is exactly what
# subprocess.run(capture_output=True) already captures with zero extra
# plumbing.
WRAPPER_TEMPLATE = '''{spec_defines}
{body}

int main(int argc, char** argv)
{{
  (void)argc; (void)argv;
  char* fargv[] = {{ {argv_list} , NULL }};
  int fargc = {argc};

  kernel_{name}(fargc, fargv);
  return 0;
}}
'''


def gen_one(kname: str, cfg: dict):
    bdir = SPEC_ROOT / cfg["bench_dir"]
    src_root = bdir / "src"

    # collect every header under src/ (excluding skip_dirs), verify basenames
    # are unique before flattening.
    headers = [h for h in src_root.rglob("*.h")
               if not (set(h.relative_to(src_root).parts[:-1]) & cfg["skip_dirs"])]
    names = [h.name for h in headers]
    dupes = {n for n in names if names.count(n) > 1}
    if dupes:
        return None, f"duplicate header basenames, can't flatten: {dupes}"

    shim_root = OUT_ROOT / kname / "SPEC_shim"
    kdir = shim_root / "kernels" / kname
    udir = shim_root / "utilities"
    kdir.mkdir(parents=True, exist_ok=True)
    udir.mkdir(parents=True, exist_ok=True)

    for h in headers:
        shutil.copy(h, kdir / h.name)
        shutil.copy(h, udir / h.name)

    entry_path = src_root / cfg["entry_file"]
    entry_text = entry_path.read_text(errors="replace")
    entry_text = rename_entry_all(entry_text, "main", f"kernel_{kname}")

    argv = cfg["argv"](bdir)
    argv_list = ", ".join(f'"{a}"' for a in argv)

    wrapper = WRAPPER_TEMPLATE.format(
        spec_defines=SPEC_DEFINES_H, body=entry_text, argv_list=argv_list,
        argc=len(argv), name=kname,
    )
    (kdir / f"{kname}.c").write_text(wrapper)

    extra = [sf for sf in cfg["sources"] if sf != cfg["entry_file"]]
    pieces = [SPEC_DEFINES_H, POLYBENCH_C.read_text()]
    for sf in extra:
        p = src_root / sf
        pieces.append(f"\n/* ==== {sf} ==== */\n" + p.read_text(errors="replace"))
    (udir / "polybench.c").write_text("\n".join(pieces))
    shutil.copy(POLYBENCH_H, udir / "polybench.h")

    return kdir / f"{kname}.c", None


def try_compile(kernel_c: Path) -> tuple:
    udir = kernel_c.parent.parent.parent / "utilities"
    kdir = kernel_c.parent
    out_bin = Path("/tmp") / f"spectest_{kernel_c.stem}"
    cmd = [CLANG, "-O3", "-std=gnu99", "-w",
           f"-I{udir}", f"-I{kdir}",
           "-DLARGE_DATASET",
           str(kernel_c), str(udir / "polybench.c"),
           "-o", str(out_bin), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180, errors="replace")
    if r.returncode != 0:
        return False, r.stderr[-3000:]
    run = subprocess.run([str(out_bin)], capture_output=True, text=True, timeout=120, errors="replace")
    return run.returncode == 0, (run.stdout[-1500:] + "\n---stderr---\n" + run.stderr[-1500:])


def main():
    TMP_DIR.mkdir(exist_ok=True)
    ok, fail = [], []
    for kname, cfg in BENCHMARKS.items():
        try:
            kernel_c, err = gen_one(kname, cfg)
        except Exception as e:
            fail.append((kname, f"generator exception: {e}"))
            continue
        if kernel_c is None:
            fail.append((kname, err))
            continue
        success, log = try_compile(kernel_c)
        if success:
            ok.append((kname, str(kernel_c)))
            print(f"OK  {kname}\n{log}\n")
        else:
            fail.append((kname, log))

    print(f"\n=== {len(ok)} compiled+ran OK ===")
    for kname, path in ok:
        print(f"  {kname} -> {path}")
    print(f"\n=== {len(fail)} failed ===")
    for kname, err in fail:
        print(f"  {kname}:")
        print("    " + (err or "").replace("\n", "\n    ")[:1500])

    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    manifest = OUT_ROOT / "manifest.txt"
    manifest.write_text("\n".join(path for _, path in ok) + ("\n" if ok else ""))
    print(f"\nmanifest written: {manifest} ({len(ok)} kernels)")


if __name__ == "__main__":
    main()
