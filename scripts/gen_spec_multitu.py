#!/usr/bin/env python3
"""
Generate multi-TU (non-unity) build manifests for C++ SPEC CPU2017 benchmarks
that gen_spec_kernels.py's unity-build approach is unsafe for -- see
docs/NAMD_CXX_BUILD_PLAN.md section 2 for why forcing a real object-oriented
C++ codebase's non-entry files into one textually-concatenated translation
unit (the trick gen_spec_kernels.py/gen_cbench_kernels.py use for C) is a much
bigger ODR-conflict risk than it was for nab_r's plain-C regex engine: file-
scoped statics/anonymous namespaces are far more common per-file in idiomatic
C++, and macro definitions from one file silently leaking into a physically-
later concatenated file affects templates/inline functions more than plain C
declarations.

Supported initially: 508.namd_r, 531.deepsjeng_r, and 541.leela_r. Per the
build-plan documents:
  - the entry file's `main` is renamed to `kernel_<name>` (same identifier-
    boundary regex as gen_spec_kernels.py::rename_entry_all -- duplicated
    here in miniature rather than imported, since scripts/ isn't a package
    and cross-script sys.path imports are more fragile than an 8-line copy)
    and a tiny wrapper main() appended that calls kernel_<name>(argc, argv)
    with a fixed argv built from the benchmark's real SPEC workload.
  - every OTHER source file is copied byte-for-byte, unmodified, as its own
    independent translation unit -- NO textual concatenation anywhere.
  - a build_manifest.json (src/build_manifest.py's schema, consumed by its
    MultiTUBuilder) is emitted describing every translation unit, the
    pinned LLVM 21 standard version, defines, and BOTH the "test" (fast
    smoke, 1 iteration) and "ref" (SPEC's official reference workload, 65
    iterations) runtime contracts.

This module deliberately does NOT touch gen_spec_kernels.py, optimize.py,
tune_param.py, tune_source.py, or configs/config.yaml -- it is a fully
separate generation path feeding src/build_manifest.py's MultiTUBuilder,
not the existing 2-TU (driver.c + polybench.c) harness those files assume.
Running this module only GENERATES files (source copies + JSON); it never
compiles or times anything itself -- see scripts/gen_spec_kernels.py's own
test-compile step for that class of behavior, intentionally not replicated
here yet (docs/NAMD_CXX_BUILD_PLAN.md section 5 covers why compiling is a
separate, later step from generating a manifest).
"""
from __future__ import annotations

import json
import os
import re
import shutil
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parents[1]

# Same env var name gen_spec_kernels.py uses, for a single consistent way to
# point every SPEC-reading script at a checkout -- default matches its
# existing default too.
SPEC_ROOT = Path(os.environ.get(
    "SPEC_CPU_ROOT", "/home/hanning/spec2017/benchspec/CPU"))

OUT_ROOT = PROJECT_ROOT / "SPEC_multitu_root"

# SPEC's own harness always compiles with -DSPEC -DNDEBUG (see
# benchspec/Makefile.defaults: CPUFLAGS = -DSPEC -DNDEBUG) -- same constant
# gen_spec_kernels.py bakes in, reused here for the same reason.
SPEC_COMMON_DEFINES = ["SPEC", "NDEBUG"]


# ── Benchmark descriptions (hand-transcribed from each benchmark's real
#    Spec/object.pm + data/<workload>/input/, same as gen_spec_kernels.py's
#    BENCHMARKS dict does for its own candidates -- see
#    docs/NAMD_CXX_BUILD_PLAN.md section 1 for namd_r's sourcing) ──────────

BENCHMARKS = {
    "namd_r": {
        "bench_dir": "508.namd_r",
        "entry_file": "spec_namd.C",
        # Spec/object.pm's @sources, minus the entry file. Order preserved
        # from object.pm purely for readability; link order doesn't matter
        # here since these compile to independent .o files.
        "sources": [
            "Compute.C", "ComputeList.C", "ComputeNonbondedFEP.C",
            "ComputeNonbondedLES.C", "ComputeNonbondedPProf.C",
            "ComputeNonbondedStd.C", "ComputeNonbondedUtil.C",
            "LJTable.C", "Molecule.C", "Patch.C", "PatchList.C",
            "ResultSet.C", "SimParameters.C", "erf.C",
        ],
        "cxx_standard": "gnu++03",
        # object.pm: $bench_cxxflags. NAMD_DISABLE_SSE matters most on an
        # x86_64 host (ComputeNonbondedBase.h's `#if defined(__SSE2__) &&
        # !defined(NAMD_DISABLE_SSE)` guards <emmintrin.h> intrinsics that
        # don't exist on aarch64 anyway, but the define is harmless there
        # and required correctness-wise on x86_64) -- see
        # docs/NAMD_CXX_BUILD_PLAN.md section 1.3.
        "benchmark_defines": [
            "SPEC_LP64",  # Makefile.defaults EXTRA_PORTABILITY, 64-bit branch
            "NAMD_DISABLE_SSE",
            "SPEC_AUTO_SUPPRESS_OPENMP",
        ],
        "link_flags": ["-lm"],
        # Real object.pm invoke(): reads data/<workload>/input/namd.in,
        # splits its TEXT CONTENT on whitespace to build argv -- the actual
        # simulation data (apoa1.input, ~8MB) is separate, shared across
        # workload sizes under data/all/input/ (SPEC's own convention: files
        # common to every workload size live in all/, per-size dirs hold
        # only what's size-specific). spec_namd.C's main() does a plain
        # fopen(argv_value) with no internal path construction, so (unlike
        # nab_r) no rundir/chdir is needed -- generation just resolves the
        # shared input file to an absolute path directly in argv.
        "shared_input_file": "data/all/input/apoa1.input",
        "workloads": {
            "test": {
                # data/test/input/namd.in's real content:
                # "--input apoa1.input --iterations 1 --output apoa1.test.output"
                "argv": ["--input", "{shared_input}", "--iterations", "1",
                        "--output", "{output_dir}/apoa1.test.output"],
                "output_files": ["{output_dir}/apoa1.test.output"],
            },
            "ref": {
                # data/refrate/input/namd.in's real content:
                # "--input apoa1.input --output apoa1.ref.output --iterations 65"
                "argv": ["--input", "{shared_input}", "--output",
                        "{output_dir}/apoa1.ref.output", "--iterations", "65"],
                "output_files": ["{output_dir}/apoa1.ref.output"],
            },
        },
        "default_workload": "test",
    },
    "deepsjeng_r": {
        "bench_dir": "531.deepsjeng_r",
        "entry_file": "sjeng.cpp",
        "sources": [
            "attacks.cpp", "bitboard.cpp", "bits.cpp", "board.cpp",
            "draw.cpp", "endgame.cpp", "epd.cpp", "generate.cpp",
            "initp.cpp", "make.cpp", "moves.cpp", "neval.cpp", "pawn.cpp",
            "preproc.cpp", "search.cpp", "see.cpp", "state.cpp",
            "ttable.cpp", "utils.cpp",
        ],
        "cxx_standard": "gnu++03",
        "benchmark_defines": [
            "SPEC_LP64", "SMALL_MEMORY", "SPEC_AUTO_SUPPRESS_OPENMP",
        ],
        "link_flags": ["-lm"],
        "workloads": {
            "test": {
                "input_file": "data/test/input/test.txt",
                "argv": ["{input}"],
            },
            "train": {
                "input_file": "data/train/input/train.txt",
                "argv": ["{input}"],
            },
            "ref": {
                "input_file": "data/refrate/input/ref.txt",
                "argv": ["{input}"],
            },
        },
        "default_workload": "test",
    },
    "leela_r": {
        "bench_dir": "541.leela_r",
        "entry_file": "Leela.cpp",
        "sources": [
            "FullBoard.cpp", "KoState.cpp", "Playout.cpp", "TimeControl.cpp",
            "UCTSearch.cpp", "GameState.cpp", "SGFParser.cpp", "Timing.cpp",
            "Utils.cpp", "FastBoard.cpp", "Matcher.cpp", "SGFTree.cpp",
            "TTable.cpp", "Zobrist.cpp", "FastState.cpp", "GTP.cpp",
            "MCOTable.cpp", "Random.cpp", "SMP.cpp", "UCTNode.cpp",
        ],
        "cxx_standard": "gnu++03",
        "benchmark_defines": ["SPEC_LP64", "SPEC_AUTO_SUPPRESS_OPENMP"],
        "link_flags": ["-lm"],
        # Vendored Boost is header-only but contains nested, duplicate
        # basenames. Preserve the complete relative tree under sources/.
        "header_trees": ["boost"],
        "workloads": {
            "test": {
                "input_file": "data/test/input/test.sgf",
                "argv": ["{input}"],
            },
            "train": {
                "input_file": "data/train/input/train.sgf",
                "argv": ["{input}"],
            },
            "ref": {
                "input_file": "data/refrate/input/ref.sgf",
                "argv": ["{input}"],
            },
        },
        "default_workload": "test",
    },
}


def rename_entry_all(text: str, entry: str, new_name: str) -> str:
    """Rename every occurrence of the bare identifier `entry` immediately
    followed by '(' to new_name -- identical algorithm to
    scripts/gen_spec_kernels.py::rename_entry_all (duplicated rather than
    imported; scripts/ has no __init__.py so it isn't a package, and an
    ad-hoc sys.path import across sibling generator scripts is more fragile
    than an 8-line copy of a pure string function)."""
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


# The entry TU keeps its own real code (just with `main` renamed) and gets a
# small appended wrapper main() -- same shape as gen_spec_kernels.py's
# WRAPPER_TEMPLATE, adapted to append-after rather than embed-in since the
# entry file here is copied whole rather than routed through a defines_h +
# unistd_include preamble (namd_r needs neither: no rundir, defines come
# from the manifest's compile-time -D flags, not source-level #ifdef).
_ENTRY_WRAPPER_TEMPLATE = '''
// ── Appended by scripts/gen_spec_multitu.py -- do not hand-edit ──────────
// Real entry point renamed kernel_{name}() above; this restores a plain
// main() that calls it with a fixed argv resolved at generation time from
// the benchmark's real SPEC workload (see BENCHMARKS[{name!r}] in
// gen_spec_multitu.py and docs/NAMD_CXX_BUILD_PLAN.md section 1.4).
int main(int argc, char** argv) {{
  // A manifest-driven run supplies its own argv and can therefore select
  // test or ref without recompiling.  A direct no-argument invocation keeps
  // the small default workload for smoke-test convenience.
  if (argc > 1) return kernel_{name}(argc, argv);
  char* fargv[] = {{ {argv_list}, NULL }};
  int fargc = {argc};
  return kernel_{name}(fargc, fargv);
}}
'''


def _format_argv_literal(argv: list) -> str:
    return ", ".join(f'"{a}"' for a in argv)


def _resolve_workload_argv(cfg: dict, workload: str, output_dir: Path) -> dict:
    """Substitute {shared_input}/{output_dir} placeholders in a workload's
    argv/output_files templates with real absolute paths, resolved at
    generation time -- so the wrapper's fixed argv never needs a rundir/
    chdir trick (see BENCHMARKS[...]['shared_input_file']'s docstring)."""
    bdir = SPEC_ROOT / cfg["bench_dir"]
    wl = cfg["workloads"][workload]
    values = {"output_dir": str(output_dir)}
    input_files = []
    if cfg.get("shared_input_file"):
        shared_input = (bdir / cfg["shared_input_file"]).resolve()
        values["shared_input"] = str(shared_input)
        input_files.append(shared_input)
    if wl.get("input_file"):
        input_file = (bdir / wl["input_file"]).resolve()
        values["input"] = str(input_file)
        input_files.append(input_file)
    fmt = lambda s: s.format(**values)
    return {
        "argv": [fmt(a) for a in wl["argv"]],
        "output_files": [fmt(p) for p in wl.get("output_files", [])],
        "input_files": input_files,
    }


def _runtime_contract(cwd: Path, kernel_name: str, argv: list, output_files: list) -> dict:
    """Shape matches src/build_manifest.py's RuntimeContract fields exactly
    (cwd/argv/stdin/output_files) so a workload dict here can be dropped
    straight into the manifest's top-level "runtime" key unchanged."""
    return {
        "cwd": str(cwd),
        "argv": [kernel_name] + argv,
        "stdin": None,
        "output_files": output_files,
    }


def gen_one(name: str, cfg: dict) -> "tuple[Optional[Path], Optional[str]]":
    """Generate SPEC_multitu_root/<name>/{sources/, build_manifest.json}.
    Returns (manifest_path, None) on success, (None, error_message) on
    failure (missing source file, etc.) -- mirrors gen_spec_kernels.py's own
    (result, error) return convention.

    Never compiles anything -- see this module's docstring for why that's a
    deliberately separate, later step.
    """
    bdir = SPEC_ROOT / cfg["bench_dir"]
    src_root = bdir / "src"
    kernel_name = f"kernel_{name}"

    out_dir = OUT_ROOT / name
    sources_dir = out_dir / "sources"
    sources_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "build_manifest.json"
    # A failed regeneration must never leave a previously successful manifest
    # looking current while its sources are only partially updated.
    manifest_path.unlink(missing_ok=True)

    # ── entry TU: rename main -> kernel_<name>, append fixed-argv wrapper ──
    entry_path = src_root / cfg["entry_file"]
    if not entry_path.is_file():
        return None, f"entry file not found: {entry_path}"
    entry_text = entry_path.read_text(errors="replace")
    entry_text = rename_entry_all(entry_text, "main", kernel_name)

    workload_name = cfg["default_workload"]
    resolved = _resolve_workload_argv(cfg, workload_name, out_dir / "run")
    missing_inputs = [path for path in resolved["input_files"] if not path.is_file()]
    if missing_inputs:
        return None, f"workload input not found: {missing_inputs[0]}"
    entry_text += _ENTRY_WRAPPER_TEMPLATE.format(
        name=name,
        argv_list=_format_argv_literal([kernel_name] + resolved["argv"]),
        argc=1 + len(resolved["argv"]),
    )
    entry_out = sources_dir / cfg["entry_file"]
    entry_out.write_text(entry_text)

    # ── every other source: copied AS-IS, byte for byte -- NO concatenation,
    #    each stays its own translation unit (the entire point of this
    #    generator vs. gen_spec_kernels.py's unity-build approach) ─────────
    copied = [cfg["entry_file"]]
    for sf in cfg["sources"]:
        sp = src_root / sf
        if not sp.is_file():
            return None, f"source file not found: {sp}"
        destination = sources_dir / sf
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sp, destination)
        copied.append(sf)

    # ── root headers stay alongside root sources. Selected vendor include
    #    subtrees (e.g. Leela's boost/) are copied recursively with their
    #    directory structure intact. No source/header text is concatenated. ─
    headers = sorted(src_root.glob("*.h"))
    header_names = {h.name for h in headers}
    dupes = {n for n in header_names if [h.name for h in headers].count(n) > 1}
    if dupes:
        return None, f"duplicate header basenames, can't flatten: {dupes}"
    for h in headers:
        shutil.copyfile(h, sources_dir / h.name)
    for relative_tree in cfg.get("header_trees", []):
        source_tree = src_root / relative_tree
        if not source_tree.is_dir():
            return None, f"header tree not found: {source_tree}"
        shutil.copytree(
            source_tree, sources_dir / relative_tree, dirs_exist_ok=True)

    # "workloads": carries BOTH the "test" and "ref" runtime contracts, in
    # the same per-field shape as "runtime" below, for forward compatibility
    # / informational use -- src/build_manifest.py::load_build_manifest only
    # parses the singular "runtime" key today (RuntimeContract has no
    # concept of multiple named workloads yet), so this is intentionally an
    # additive, silently-ignored-by-the-current-loader extra key rather than
    # a change to that schema.
    workloads_out = {}
    for wl_name in cfg["workloads"]:
        wl_resolved = _resolve_workload_argv(cfg, wl_name, out_dir / "run")
        missing_inputs = [path for path in wl_resolved["input_files"]
                          if not path.is_file()]
        if missing_inputs:
            return None, f"workload input not found: {missing_inputs[0]}"
        workloads_out[wl_name] = _runtime_contract(
            out_dir / "run", kernel_name, wl_resolved["argv"], wl_resolved["output_files"])

    # ── build_manifest.json (src/build_manifest.py's schema) ──────────────
    # "sources" paths are resolved by src/build_manifest.py's
    # load_build_manifest() relative to the MANIFEST FILE's own directory
    # (out_dir), not relative to sources_dir -- so every entry needs the
    # "sources/" prefix to actually point at the files just copied above.
    manifest = {
        "version": 1,
        "name": name,
        "sources": [f"sources/{f}" for f in [cfg["entry_file"]] + list(cfg["sources"])],
        "include_dirs": ["sources"],
        "defines": SPEC_COMMON_DEFINES + list(cfg["benchmark_defines"]),
        "compile_flags": [],
        "link_flags": list(cfg["link_flags"]),
        "cxx_standard": cfg["cxx_standard"],
        "default_workload": workload_name,
        "runtime": _runtime_contract(
            out_dir / "run", kernel_name, resolved["argv"], resolved["output_files"]),
        "workloads": workloads_out,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest_path, None


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=list(BENCHMARKS), default=None,
                        help="generate only this benchmark (default: all configured)")
    args = parser.parse_args()

    targets = [args.only] if args.only else list(BENCHMARKS)
    ok, failed = [], []
    for name in targets:
        manifest_path, err = gen_one(name, BENCHMARKS[name])
        if err:
            failed.append((name, err))
            print(f"  {name}: FAILED -- {err}")
        else:
            ok.append(name)
            print(f"  {name}: manifest written -> {manifest_path}")

    print(f"\n{len(ok)} manifest(s) generated, {len(failed)} failed.")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
