#!/usr/bin/env python3
"""
Generate one self-contained PolyBench-shaped kernel per cBench program+variant.

Design mirrors scripts/gen_tsvc_kernels.py: each generated kernel presents as
a normal PolyBench kernel (a `kernel_<name>()` function + a private
utilities/polybench.c) so the existing COMET harness runs against it
completely unmodified.

Per program:
  - If the program uses the CK `ctuning-rtl.c` + `main1(...)` convention
    (most multi-file programs), we DROP ctuning-rtl.c entirely and rename
    `main1` -> `kernel_<name>` in the file that defines it.
  - If the program has a plain `main(argc, argv)` (susan, dijkstra,
    stringsearch2, bitcount, adpcm-c/d, mad, pgp), we rename that.
  - Any other source files belonging to the program are appended into a
    program-private copy of polybench.c (kept as a *separate* translation
    unit list passed to clang -- see NOTE below), living under
    <program>/CBench_shim/utilities/. Because POLYBENCH_DIR_NAMES matches by
    directory *name* (not global uniqueness), every program gets its own
    "CBench_shim" ancestor directory, so find_polybench_utilities resolves
    each program's own utilities/ independently -- no cross-program
    interference, no change to optimize.py/tune_param.py/tune_source.py.

  NOTE on multi-file linking: compile_binary()/compile_c() call sites always
  compile exactly [kernel_src, polybench_c] as two translation units -- never
  more. For programs with >2 source files we cannot pass each remaining file
  as its own clang argument without touching that call-site list (which
  would mean editing the harness). Instead we place the *textual content* of
  every remaining file after the real polybench.c content inside the
  program-private polybench.c copy, `#include`-guarded by the originals'
  own header guards. This is a unity build of exactly the OTHER files (never
  the kernel file itself), which keeps every one of those files' `static`
  symbols in a single new translation unit together -- generally safe for
  these historically single-purpose benchmark sources, but not guaranteed;
  each generated kernel is therefore *actually test-compiled* by this script
  and only kept if clang succeeds. Failures are reported, not silently
  skipped.

Correctness: no polybench.h, no framework macros -- the harness (see
src/correctness.py) hashes/numeric-compares whatever a kernel prints on its
real stdout, captured directly by the process's own stdout pipe (no shared
tmp file to race on). Most cBench programs (dijkstra, bzip2 -c, adpcm-c/d,
crc32, sha, qsort1, patricia, ...) already write their real product output
straight to stdout -- for those the wrapper does nothing extra. A handful
(tiff2bw/tiff2dither/tiff2median/tiff2rgba, consumer-jpeg-c, ...) take an
explicit output-*file* argv token and write their product there instead of
to stdout; for those the wrapper reads that file back and streams it onto
its own stdout right after the call, so the harness still only ever needs
to look at one place (this process's own stdout, captured process-isolated
by subprocess.run) regardless of which convention the underlying program
uses. That file path is namespaced by getpid() so two invocations (a
reference run and a candidate run, or any future concurrent evaluation)
can never collide on the same path.
"""
import json
import re
import shutil
import subprocess
from pathlib import Path

CBENCH_ROOT = Path("/home/hanning/cbench/program")
DATASET_ROOT = Path("/home/hanning/ctuning-datasets-min/dataset")
POLYBENCH_C = Path("/home/hanning/comet/PolyBenchC_no_rag/utilities/polybench.c")
POLYBENCH_H = Path("/home/hanning/comet/PolyBenchC_no_rag/utilities/polybench.h")
OUT_ROOT = Path("/home/hanning/comet/CBench_shim_root")
CLANG = "/usr/bin/clang-11"

# program -> variant -> dataset resolution.
# Each entry resolves the CK $#dataset_...#$ placeholders in that variant's
# run_cmd_main template (see meta.json) to real files under ctuning-datasets-min.
DATASETS = {
    ("cbench-automotive-bitcount", "default"): {"number": "1125000"},
    ("cbench-automotive-qsort1", "default"): {"file": "cdataset-qsort-0001/data.txt"},
    ("cbench-automotive-susan", "corners"): {"file": "image-pgm-0001/data.pgm"},
    ("cbench-automotive-susan", "edges"): {"file": "image-pgm-0001/data.pgm"},
    ("cbench-automotive-susan", "smoothing"): {"file": "image-pgm-0001/data.pgm"},
    ("cbench-bzip2", "decode"): {"file": "bzip2-0001/data.bz2"},
    ("cbench-bzip2", "encode"): {"file": "bzip2-0001/data"},
    ("cbench-consumer-jpeg-c", "encode"): {"file": "image-ppm-0001/data.ppm"},
    ("cbench-consumer-jpeg-d", "decode"): {"file": "image-jpeg-0001/data.jpg"},
    ("cbench-consumer-lame", "default"): {"file": "audio-wav-0001/data.wav"},
    ("cbench-consumer-mad", "default"): {"file": "audio-mp3-0001/data.mp3"},
    ("cbench-consumer-tiff2bw", "convert"): {"file": "image-tiff-0001/data.tiff"},
    ("cbench-consumer-tiff2dither", "convert"): {"file": "image-tiff-0001/data.tiff"},
    ("cbench-consumer-tiff2median", "convert"): {"file": "image-tiff-0001/data.tiff"},
    ("cbench-consumer-tiff2rgba", "convert"): {"file": "image-tiff-0001/data.tiff"},
    ("cbench-network-dijkstra", "default"): {"file": "cdataset-dijkstra-0001/data.txt"},
    ("cbench-network-patricia", "default"): {"file": "cdataset-patricia-0001/data.txt"},
    ("cbench-office-stringsearch2", "default"): {
        "file": "txt-0001/data.txt", "file_1": "txt-0001/data.s.txt",
    },
    ("cbench-security-pgp", "decode"): {"file": "pgp-0001/data.pgp"},
    ("cbench-security-rijndael", "decode"): {"file": "enc-0001/data.enc"},
    ("cbench-security-rijndael", "encode"): {"file": "enc-0001/data.enc"},
    ("cbench-security-sha", "default"): {"file": "txt-0001/data.txt"},
    ("cbench-telecom-adpcm-c", "encode"): {"file": "pcm-0001/data.pcm"},
    ("cbench-telecom-adpcm-d", "decode"): {"file": "adpcm-0001/data.adpcm"},
    ("cbench-telecom-crc32", "default"): {"file": "txt-0001/data.txt"},
    ("cbench-telecom-gsm", "default"): {"file": "au-0001/data.au"},
}
# blowfish: no main()/main1() found in its 7 source files (library-only
# checkout, no driver) -- not generatable, intentionally omitted.
# pgp "encode": ambiguous dataset_tags (['', 'dataset']) -- omitted.

def find_entry(prog_dir: Path, source_files: list):
    """Return (entry_func, file_with_entry, needs_print_arg, uses_rtl, zero_arg)."""
    rtl = prog_dir / "ctuning-rtl.c"
    if rtl.exists():
        for sf in source_files:
            p = prog_dir / sf
            if sf == "ctuning-rtl.c" or not p.exists():
                continue
            txt = p.read_text(errors="replace")
            # Arity is taken from the *definition* in sf, not ctuning-rtl.c's
            # forward declaration -- several cbench programs' rtl prototype
            # and real definition disagree on the trailing `int print` param.
            def_m = re.search(r"\bmain1\s*\(\s*\w+\s+argc[^)]*\)", txt)
            if def_m:
                needs_print = bool(re.search(r",\s*int\s+\w+\s*\)$", def_m.group(0)))
                return "main1", sf, needs_print, True, False
        return None, None, False, True, False
    for sf in source_files:
        p = prog_dir / sf
        if not p.exists():
            continue
        txt = p.read_text(errors="replace")
        if re.search(r"\bmain\s*\(\s*int\s+argc", txt) or re.search(r"\bmain\s*\(\s*void\s*\)", txt):
            return "main", sf, False, False, False
        if re.search(r"\bmain\s*\(\s*\)\s*\{", txt):
            return "main", sf, False, False, True
    return None, None, False, False, False


def rename_entry(text: str, entry: str, new_name: str) -> str:
    # Match only a real function-definition signature (entry immediately
    # followed by "(<type> argc", "(void)" or "()") -- not bare mentions of
    # the name inside comments/strings (e.g. "/* main(argc, argv) */").
    pattern = (r"(?<![A-Za-z_0-9])" + re.escape(entry)
               + r"(\s*\(\s*(?:\w+\s+argc|void\s*\)|\)))")
    matches = list(re.finditer(pattern, text))
    assert len(matches) == 1, f"expected exactly 1 signature match for {entry}, got {len(matches)}"
    m = matches[0]
    return text[:m.start()] + new_name + m.group(1) + text[m.end():]


def build_argv(name: str, variant: str, template: str, resolved: dict, workdir: Path):
    """Turn a CK run_cmd_main template into a C argv[] initializer list + stdin path (or None)."""
    tokens = template.split()
    assert tokens[0] == "$#BIN_FILE#$"
    tokens = tokens[1:]
    stdin_file = None
    if tokens and tokens[0] == "<":
        stdin_file = str(DATASET_ROOT / resolved["file"])
        tokens = tokens[1:]

    def resolve_tok(tok):
        tok = tok.replace("$#dataset_path#$", str(DATASET_ROOT / Path(resolved.get("file", "x")).parent) + "/")
        tok = tok.replace("$#dataset_filename_1#$", Path(resolved.get("file_1", "")).name)
        tok = tok.replace("$#dataset_filename#$", Path(resolved.get("file", "")).name)
        tok = tok.replace("$#dataset_number#$", resolved.get("number", "0"))
        return tok

    argv = [f"kernel_{name}"] + [resolve_tok(t) for t in tokens]
    # any bare relative filename left over (e.g. "tmp-output.tmp") -> put under workdir
    fixed = []
    for i, a in enumerate(argv):
        if i > 0 and "/" not in a and a not in ("-c", "-e", "-s", "-d", "-z", "-k", "-f",
                                                  "-dct", "int", "-progressive", "-opt",
                                                  "-outfile", "-ppm", "-c", "none",
                                                  "d", "e", "-o", "-s", "test", "-fps") \
                and not a.startswith("1234567890") and not re.match(r"^-", a):
            a = str(workdir / a)
        fixed.append(a)
    return fixed, stdin_file


# Generic wrapper -- no polybench.h, no framework macros. Timing is
# external wall-clock (src/build_utils.py::run_timing) and correctness is
# checked against this process's own real stdout (src/correctness.py) --
# see docs/GENERIC_HARNESS_DESIGN.md and the module docstring above for why
# out_path_decl/cat_output exist for the minority of programs that write
# their product to a file instead of stdout.
WRAPPER_TEMPLATE = '''
#include <stdio.h>
#include <unistd.h>

{body}

static void _cat_file_to_stdout(const char* path)
{{
  FILE* f = fopen(path, "rb");
  if (!f) return;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
  fclose(f);
  remove(path);
}}

int main(int argc, char** argv)
{{
{out_path_decl}
  char* fargv[] = {{ {argv_list} , NULL }};
  int fargc = {argc};
{stdin_setup}
  kernel_{name}({call_args});
{cat_output}
  return 0;
}}
'''


def gen_one(prog_name: str, variant: str, resolved: dict):
    prog_dir = CBENCH_ROOT / prog_name
    meta = json.loads((prog_dir / ".cm" / "meta.json").read_text())
    source_files = meta.get("source_files", [])
    template = meta["run_cmds"][variant]["run_time"]["run_cmd_main"]
    if prog_name == "cbench-bzip2":
        # Without -c, bzip2 -d/-z writes its real output to a file derived
        # from the input filename (not stdout), so our stdout-redirect-based
        # checksum would always see an empty capture. -c keeps -d/-z's
        # behavior otherwise identical but routes the actual (de)compressed
        # bytes to stdout, where our redirect captures them for real.
        template = template.replace("$#BIN_FILE#$", "$#BIN_FILE#$ -c")

    entry, entry_file, needs_print, uses_rtl, zero_arg = find_entry(prog_dir, source_files)
    if entry is None:
        return None, "no main()/main1() entry point found"

    kname = prog_name.replace("cbench-", "").replace("-", "_")
    if len(meta.get("run_cmds", {})) > 1:
        kname = f"{kname}_{variant}"

    shim_root = OUT_ROOT / f"{prog_name}_{variant}" / "CBench_shim"
    kdir = shim_root / "kernels" / kname
    udir = shim_root / "utilities"
    kdir.mkdir(parents=True, exist_ok=True)
    udir.mkdir(parents=True, exist_ok=True)

    entry_text = (prog_dir / entry_file).read_text(errors="replace")
    entry_text = rename_entry(entry_text, entry, f"kernel_{kname}")

    argv, stdin_file = build_argv(kname, variant, template, resolved, Path("/home/hanning/comet/tmp"))
    out_idx = None
    for i, a in enumerate(argv[1:], start=1):
        if a.endswith(".tmp") or "tmp-output" in a:
            out_idx = i
            break

    if out_idx is not None:
        # Program writes its real product to a file it opens itself (e.g.
        # tiff2bw's output path), not to stdout. Namespace that path by
        # getpid() so a reference run and a candidate run (or any future
        # concurrent evaluation) can never collide on the same path, and
        # have the wrapper read it back onto its own stdout right after the
        # call -- the harness then only ever needs to look in one place.
        argv_list = ", ".join(
            "out_path" if i == out_idx else f'"{a}"' for i, a in enumerate(argv))
        out_path_decl = (
            "  char out_path[512];\n"
            f'  snprintf(out_path, sizeof(out_path), "/home/hanning/comet/tmp/{kname}_out_%d.tmp", (int)getpid());\n'
        )
        cat_output = "  _cat_file_to_stdout(out_path);\n"
    else:
        # No explicit output-file token (e.g. dijkstra, bzip2 -c, adpcm-c/d,
        # crc32, sha, qsort1, patricia) -- its real output is already
        # whatever it writes to its own stdout, which the harness captures
        # directly. Nothing extra needed.
        argv_list = ", ".join(f'"{a}"' for a in argv)
        out_path_decl = ""
        cat_output = ""

    if zero_arg:
        call_args = ""
    elif uses_rtl:
        call_args = "fargc, fargv" + (", 1" if needs_print else "")
    else:
        call_args = "fargc, fargv"

    stdin_setup = ""
    if stdin_file:
        stdin_setup = f'  if (!freopen("{stdin_file}", "r", stdin)) return 1;\n'

    wrapper = WRAPPER_TEMPLATE.format(
        body=entry_text, argv_list=argv_list, argc=len(argv),
        out_path_decl=out_path_decl, cat_output=cat_output,
        stdin_setup=stdin_setup, name=kname, call_args=call_args,
    )
    (kdir / f"{kname}.c").write_text(wrapper)

    # Build this program's private polybench.c: real polybench.c + every
    # OTHER source file (not entry_file, not ctuning-rtl.c) appended.
    extra = [sf for sf in source_files if sf not in (entry_file, "ctuning-rtl.c")]
    pieces = [POLYBENCH_C.read_text()]
    for sf in extra:
        p = prog_dir / sf
        if p.suffix == ".c" and p.exists():
            pieces.append(f"\n/* ==== {sf} ==== */\n" + p.read_text(errors="replace"))
    (udir / "polybench.c").write_text("\n".join(pieces))
    shutil.copy(POLYBENCH_H, udir / "polybench.h")

    # copy headers (and any non-.c extra files clang needs via #include) so
    # -I{source_dir} finds them from the kernel file, and -I{utils} finds
    # them for the merged polybench.c
    for f in prog_dir.iterdir():
        if f.suffix in (".h",) or (f.is_file() and "." not in f.name):
            shutil.copy(f, kdir / f.name)
            shutil.copy(f, udir / f.name)

    return kdir / f"{kname}.c", None


def try_compile(kernel_c: Path) -> tuple:
    udir = kernel_c.parent.parent.parent / "utilities"
    kdir = kernel_c.parent
    out_bin = Path("/tmp") / f"cbenchtest_{kernel_c.stem}"
    cmd = [CLANG, "-O3", "-std=gnu99", "-w",
           f"-I{udir}", f"-I{kdir}",
           "-DLARGE_DATASET",
           str(kernel_c), str(udir / "polybench.c"),
           "-o", str(out_bin), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, errors="replace")
    return r.returncode == 0, r.stderr[-2000:]


def main():
    Path("/home/hanning/comet/tmp").mkdir(exist_ok=True)
    ok, fail = [], []
    for (prog, variant), resolved in DATASETS.items():
        try:
            kernel_c, err = gen_one(prog, variant, resolved)
        except Exception as e:
            fail.append((prog, variant, f"generator exception: {e}"))
            continue
        if kernel_c is None:
            fail.append((prog, variant, err))
            continue
        success, stderr = try_compile(kernel_c)
        if success:
            ok.append((prog, variant, str(kernel_c)))
        else:
            fail.append((prog, variant, stderr))

    print(f"\n=== {len(ok)} compiled OK ===")
    for prog, variant, path in ok:
        print(f"  {prog}/{variant} -> {path}")
    print(f"\n=== {len(fail)} failed ===")
    for prog, variant, err in fail:
        print(f"  {prog}/{variant}:")
        print("    " + (err or "").replace("\n", "\n    ")[:500])

    manifest = Path("/home/hanning/comet/CBench_shim_root/manifest.txt")
    manifest.write_text("\n".join(path for _, _, path in ok) + "\n")
    print(f"\nmanifest written: {manifest} ({len(ok)} kernels)")


if __name__ == "__main__":
    main()
