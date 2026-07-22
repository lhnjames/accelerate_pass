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

Each generated kernel is actually test-compiled with the pinned LLVM 21
toolchain (CLANG constant below) and only kept in the manifest if it
succeeds.

Candidates covered so far (pure C, single `main`, small-to-medium file
count -- see docs/SPEC2017_STATUS.md for the full 43-benchmark survey and
why the rest are deferred):
    505.mcf_r   (11 files, network simplex solver)
    519.lbm_r   ( 2 files, lattice-Boltzmann fluid dynamics)
In progress, NOT compiling yet (do not add to manifest expectations):
    544.nab_r    -- got past the "missing file"/argv-chdir issues (see
                    "rundir" below and the .c/.ih flattening in gen_one),
                    but its regex-alpha/ subsystem was never designed for
                    unity-building: regex2.h has no include guard (fine
                    across separate TUs, breaks when regcomp.c AND
                    regexec.c's bodies both land in one TU), and at least
                    one real symbol collision (a file-scoped `static
                    REAL_T dist(ATOM_T*, ATOM_T*)` helper in one file vs
                    an unrelated extern `dist(MOLECULE_T*, char[], char[])`
                    declared in another -- two different functions that
                    happen to share a name, only safe under separate
                    compilation) plus several stale/mismatched forward-
                    declarations (reducerror, select_atoms, get) that
                    various nab source files carry locally and never
                    actually got cross-checked before. Needs either format
                    per-symbol renaming/guard patches at generation time,
                    or (bigger change) compiling regex-alpha's files as
                    genuinely separate translation units instead of unity-
                    building them -- not attempted, would touch the
                    generic single-TU assumption gen_one() and the rest of
                    this harness both currently make.
Not yet done (documented, not attempted here):
    557.xz_r     -- two `main()`s (spec.c harness + pxz.c), ~70 files.
    538.imagick_r, 500.perlbench_r, 502.gcc_r -- SPEC harness
                    (spec.c/SpecMain) dispatch pattern, large file counts.
    Fortran-language and CXX-language benchmarks -- out of scope for this
    C-only, clang -mllvm-flag-driven harness.

nab_r's argv[1] handling ("rundir" config key): nabmd.c's main() builds its
input filename as `strcat(argv[1], "/"); strcat(argv[1], argv[1]); strcat
(".pdb")` -- i.e. literally `<argv1>/<argv1>.pdb` -- which only resolves if
argv[1] is a short relative name ("hkrdenq") looked up in the CURRENT
WORKING DIRECTORY, unlike mcf_r/lbm_r where every path could just be made
absolute in argv itself with no chdir needed. A "rundir" config entry
copies the needed data/test/input/<dirname>/ subtree into the shim's own
tree ONCE at generation time (not at every run -- doing it at runtime
inside the wrapper would leak into the timed region) and the wrapper does
one cheap chdir() into that directory before calling kernel_<name>, with
argv left as the plain short name SPEC's own harness would pass.
"""
import argparse
import os
import re
import shutil
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
SPEC_ROOT = Path(os.environ.get(
    "SPEC_CPU_ROOT", "/home/hanning/spec2017/benchspec/CPU"))
DATA_ROOT = SPEC_ROOT
POLYBENCH_C = PROJECT_ROOT / "PolyBenchC_no_rag/utilities/polybench.c"
POLYBENCH_H = PROJECT_ROOT / "PolyBenchC_no_rag/utilities/polybench.h"
OUT_ROOT = PROJECT_ROOT / "SPEC_shim_root"
TMP_DIR = PROJECT_ROOT / "tmp"
CLANG = os.environ.get(
    "COMET_CLANG_21", str(PROJECT_ROOT / "scripts/toolchain/clang-21"))

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
    "nab_r": {
        "bench_dir": "544.nab_r",
        "entry_file": "nabmd.c",
        # from Spec/object.pm's @sources; eff.c/sff2.c are pulled in via
        # textual #include from inside sff.c, not compiled separately (see
        # the .c-flattening comment in gen_one) -- but sff.c only skips the
        # `#include "sff2.c"` line (a file that doesn't exist in this SPEC
        # distribution at all -- presumably an optional perf-library variant
        # never shipped) when NOPERFLIB is defined, hence extra_defines
        # below matching object.pm's own $bench_flags for this benchmark.
        "sources": [
            "nabmd.c", "sff.c", "nblist.c", "prm.c", "memutil.c", "molio.c",
            "molutil.c", "errormsg.c", "binpos.c", "rand2.c",
            "select_atoms.c", "reslib.c", "database.c", "traceback.c",
            "chirvol.c", "specrand/specrand.c", "regex-alpha/regcomp.c",
            "regex-alpha/regerror.c", "regex-alpha/regexec.c",
            "regex-alpha/regfree.c",
        ],
        "skip_dirs": set(),
        # data/test/input/control: "hkrdenq 1930344093 1000" (dirname, PRNG
        # seed, MD step count) -- see Spec/object.pm's invoke(). dirname
        # stays a short relative name (see module docstring); "rundir"
        # below stages the directory it needs to resolve against.
        "argv": lambda bdir: ["kernel_nab_r", "hkrdenq", "1930344093", "1000"],
        "rundir": lambda bdir: (bdir / "data/test/input/hkrdenq", "hkrdenq"),
        "extra_defines": ["NOPERFLIB", "NOREDUCE", "SPEC_AUTO_SUPPRESS_OPENMP"],
        # nab's source files were always compiled separately -- each file's
        # local forward declarations of functions actually DEFINED in some
        # other file were never cross-checked by a linker that verifies
        # signatures (C doesn't require that), so several drifted, and one
        # pair of unrelated static helpers happen to share a name. Under
        # unity-build these become real conflicts. Two categories, two fixes:
        #  - reducerror/select_atoms: molio.c's/prm.c's LOCAL prototype has
        #    the wrong return type vs. the real definition (sff.c's
        #    reducerror returns int; select_atoms.c's select_atoms returns
        #    int) -- just correct the stale local prototype to match.
        #  - dist: molio.c has its OWN static (file-private) `dist(ATOM_T*,
        #    ATOM_T*)` helper (point-to-point distance) that is a
        #    completely different function from molutil.c's external
        #    `dist(MOLECULE_T*, char[], char[])` (distance between two
        #    named atom-expression selections) -- they only collide because
        #    they share a name. Since molio.c's version is static (used
        #    only within molio.c), rename just that one, decl+def+2 call
        #    sites, to something that can't collide.
        #  - get: molutil.c and prm.c each independently wrote their own
        #    static char *get(size_t) malloc-wrapper helper -- same
        #    signature, different (copy-pasted) functions. molutil.c's has
        #    only one call site, so rename that one rather than prm.c's ~50.
        "text_fixups": {
            "molio.c": [
                ("void reducerror(int);", "int reducerror(int);"),
                ("void select_atoms(MOLECULE_T *, char[]);",
                 "int select_atoms(MOLECULE_T *, char[]);"),
                ("static REAL_T dist(ATOM_T *, ATOM_T *);",
                 "static REAL_T molio_dist(ATOM_T *, ATOM_T *);"),
                ("static REAL_T dist(ATOM_T * ap1, ATOM_T * ap2)",
                 "static REAL_T molio_dist(ATOM_T * ap1, ATOM_T * ap2)"),
                ("res2->r_resname, ap2->a_atomname,\n                                    dist(ap1, ap2));",
                 "res2->r_resname, ap2->a_atomname,\n                                    molio_dist(ap1, ap2));"),
                ("d = dist(ap1, ap2);", "d = molio_dist(ap1, ap2);"),
            ],
            "prm.c": [
                ("void reducerror(int);", "int reducerror(int);"),
            ],
            "molutil.c": [
                ("static char * get(size)\nsize_t\tsize;",
                 "static char * molutil_get(size)\nsize_t\tsize;"),
                ("iptmp = (int *) get(sizeof(int)*12*prm->Natom);",
                 "iptmp = (int *) molutil_get(sizeof(int)*12*prm->Natom);"),
            ],
        },
    },
    "specrand_ir": {
        "bench_dir": "999.specrand_ir",
        "entry_file": "main.c",
        "sources": ["main.c", "specrand-common/specrand.c"],
        "skip_dirs": set(),
        # data/test/input/control. The ref workload (1255432124, 234923) is
        # reserved for final remote measurements; generation uses test input
        # only as a compile/run integration gate.
        "argv": lambda bdir: ["kernel_specrand_ir", "324342", "24239"],
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
{unistd_include}{body}

int main(int argc, char** argv)
{{
  (void)argc; (void)argv;
{chdir_call}  char* fargv[] = {{ {argv_list} , NULL }};
  int fargc = {argc};

  kernel_{name}(fargc, fargv);
  return 0;
}}
'''


def gen_one(kname: str, cfg: dict):
    bdir = SPEC_ROOT / cfg["bench_dir"]
    src_root = bdir / "src"

    # collect every header under src/ (excluding skip_dirs), verify basenames
    # are unique before flattening. Some codebases (nab_r's BSD regex
    # engine) use ".ih" ("inline header") for #include-only fragment files
    # alongside ordinary ".h" -- both need to be on the include path.
    headers = [h for h in list(src_root.rglob("*.h")) + list(src_root.rglob("*.ih"))
               + list(src_root.rglob("*.inc"))
               if not (set(h.relative_to(src_root).parts[:-1]) & cfg["skip_dirs"])]
    names = [h.name for h in headers]
    dupes = {n for n in names if names.count(n) > 1}
    if dupes:
        return None, f"duplicate header basenames, can't flatten: {dupes}"

    # Some benchmarks #include a helper .c file textually from inside another
    # .c file (nab_r's sff.c does `#include "eff.c"`) rather than compiling
    # it as its own translation unit -- eff.c is deliberately NOT in this
    # benchmark's "sources" list (it'd be a duplicate-symbol link error if it
    # were both #include'd AND compiled separately), but it still needs to be
    # on the include path for that #include to resolve. Flatten every .c
    # file the same way headers are flattened (skip_dirs, uniqueness check)
    # so this resolves generically instead of hunting down each case by hand
    # per benchmark.
    all_c_files = [c for c in src_root.rglob("*.c")
                   if not (set(c.relative_to(src_root).parts[:-1]) & cfg["skip_dirs"])]
    c_names = [c.name for c in all_c_files]
    c_dupes = {n for n in c_names if c_names.count(n) > 1}
    if c_dupes:
        return None, f"duplicate .c basenames, can't flatten: {c_dupes}"

    shim_root = OUT_ROOT / kname / "SPEC_shim"
    kdir = shim_root / "kernels" / kname
    udir = shim_root / "utilities"
    kdir.mkdir(parents=True, exist_ok=True)
    udir.mkdir(parents=True, exist_ok=True)

    # Some headers (nab_r's regex-alpha/regex2.h) have no include guard --
    # harmless under normal separate-TU compilation (each .c file that
    # #includes it gets its own copy, processed once), but this generator
    # unity-builds every "sources" file into one polybench.c TU, so if TWO
    # of them #include the same unguarded header, its content -- struct/
    # typedef defs included -- gets reprocessed a second time in the SAME
    # TU, which for typedefs is a hard "redefinition with different types"
    # error even when the text is byte-identical (C doesn't consider two
    # separately-parsed struct definitions the same type). Inject a guard
    # for any .h/.ih file that doesn't already have one; .inc files are
    # deliberately left alone -- nab_r's engine.inc is #include'd twice on
    # purpose (regexec.c wraps it with different macros each time to
    # generate two engine variants from one template), so guarding it would
    # break that by design. engine.ih is the same story one level down: it's
    # textually #include'd FROM INSIDE engine.inc (not from regexec.c
    # directly), so it inherits the same "processed twice, once per macro
    # state" requirement -- it supplies the forward declarations of
    # backref/fast/dissect/etc *under whichever name the active SNAMES/
    # LNAMES macros rename them to*. Guarding it silently no-ops the SECOND
    # inclusion, so the second pass's renamed functions (lbackref, lfast)
    # never get a prototype before their first use, use-before-declare then
    # makes them implicitly `extern`, and the real `static` definition later
    # in the file conflicts with that -- exactly the "static declaration of
    # 'lbackref' follows non-static declaration" error this was blocking on.
    _NEVER_GUARD = {"engine.inc", "engine.ih"}
    for h in headers:
        text = h.read_text(errors="replace")
        if (h.name not in _NEVER_GUARD and h.suffix != ".inc"
                and "ifndef" not in text[:200] and "pragma once" not in text[:200]):
            guard = f"_GEN_SPEC_GUARD_{h.stem.upper().replace('-', '_')}_H_"
            text = f"#ifndef {guard}\n#define {guard}\n{text}\n#endif /* {guard} */\n"
        (kdir / h.name).write_text(text)
        (udir / h.name).write_text(text)

    # Flatten .c files too (see the comment above all_c_files) so any
    # #include "helper.c" from within a compiled source resolves. Skip
    # ones that are cfg["sources"] members or the entry file -- those get
    # written explicitly below (concatenated into polybench.c, or as the
    # wrapper's own body) and copying them here too would be redundant,
    # not harmful, but pointless.
    _sources_and_entry = set(cfg["sources"]) | {cfg["entry_file"]}
    for c in all_c_files:
        rel = str(c.relative_to(src_root))
        if rel in _sources_and_entry:
            continue
        shutil.copy(c, kdir / c.name)
        shutil.copy(c, udir / c.name)

    entry_path = src_root / cfg["entry_file"]
    entry_text = entry_path.read_text(errors="replace")
    entry_text = rename_entry_all(entry_text, "main", f"kernel_{kname}")

    argv = cfg["argv"](bdir)
    argv_list = ", ".join(f'"{a}"' for a in argv)

    defines_h = SPEC_DEFINES_H + "".join(
        f"#ifndef {d}\n#define {d} 1\n#endif\n" for d in cfg.get("extra_defines", []))

    # "rundir": some benchmarks (nab_r) build filenames as a short relative
    # name joined against argv[1] itself, which only resolves against a
    # specific CWD -- stage that directory ONCE here (generation time), the
    # wrapper just chdir()s into it before calling kernel_<name> (cheap,
    # not part of the thing being timed's own I/O).
    chdir_call = ""
    unistd_include = ""
    if "rundir" in cfg:
        src_dir, dirname = cfg["rundir"](bdir)
        rundir_root = shim_root / "rundir"
        dest_dir = rundir_root / dirname
        if dest_dir.exists():
            shutil.rmtree(dest_dir)
        shutil.copytree(src_dir, dest_dir)
        unistd_include = "#include <unistd.h>\n"
        chdir_call = (
            f'  if (chdir("{rundir_root}") != 0) {{ perror("chdir"); return 1; }}\n'
        )

    wrapper = WRAPPER_TEMPLATE.format(
        spec_defines=defines_h, unistd_include=unistd_include, body=entry_text,
        chdir_call=chdir_call, argv_list=argv_list,
        argc=len(argv), name=kname,
    )
    (kdir / f"{kname}.c").write_text(wrapper)

    extra = [sf for sf in cfg["sources"] if sf != cfg["entry_file"]]
    pieces = [defines_h, POLYBENCH_C.read_text()]
    fixups = cfg.get("text_fixups", {})
    for sf in extra:
        p = src_root / sf
        text = p.read_text(errors="replace")
        for old, new in fixups.get(sf, []):
            n = text.count(old)
            if n != 1:
                return None, (f"text_fixup for {sf} expected exactly 1 match of "
                              f"{old!r}, found {n} -- source may have changed, "
                              f"re-check the fixup")
            text = text.replace(old, new)
        pieces.append(f"\n/* ==== {sf} ==== */\n" + text)
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=list(BENCHMARKS), default=None,
                        help="generate only this benchmark (default: all configured). "
                             "Useful when SPEC_CPU_ROOT is a partial checkout that "
                             "doesn't have every benchmark's src/ -- e.g. this repo's "
                             "local sandbox copy only has 999.specrand_ir's sources, "
                             "not mcf_r/lbm_r/nab_r's, so running unscoped there always "
                             "fails those three with 'No such file or directory'.")
    args = parser.parse_args()
    targets = {args.only: BENCHMARKS[args.only]} if args.only else BENCHMARKS

    TMP_DIR.mkdir(exist_ok=True)
    ok, fail = [], []
    for kname, cfg in targets.items():
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
    # Merge, don't overwrite: a scoped `--only` run (or a run against a
    # partial SPEC_CPU_ROOT checkout that's missing other benchmarks' src/)
    # must not destroy manifest entries for benchmarks this invocation never
    # touched. Only replace the line(s) for benchmarks actually in `targets`
    # this run -- if one of them now fails, its stale entry is correctly
    # dropped ("only kept in the manifest if it succeeds", per this module's
    # own docstring); if it succeeds, its entry is refreshed.
    existing_lines = (manifest.read_text().splitlines() if manifest.exists() else [])
    existing_lines = [l for l in existing_lines if l.strip()]

    def _kname_from_manifest_line(line: str) -> "str | None":
        m = re.search(r"/kernels/([^/]+)/", line)
        return m.group(1) if m else None

    kept = [l for l in existing_lines if _kname_from_manifest_line(l) not in targets]
    new_lines = [str(Path(path).relative_to(PROJECT_ROOT)) for _, path in ok]
    manifest.write_text(
        "\n".join(kept + new_lines) + ("\n" if (kept or new_lines) else ""))
    print(f"\nmanifest written: {manifest} "
         f"({len(new_lines)} kernel(s) from this run, {len(kept)} unchanged from before)")


if __name__ == "__main__":
    main()
