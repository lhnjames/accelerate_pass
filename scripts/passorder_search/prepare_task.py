#!/usr/bin/env python3
"""Set up a pass-order-search scratch dir for one comet benchmark program.

Usage: prepare_task.py <program_rel_path> <scratch_dir> [pin_cpu]

Writes into scratch_dir: kernel.c (+ siblings), baseline.json (baseline_ms,
correctness_mode, ref_bin path, utils/source_dir).
"""
import sys, shutil, json
from pathlib import Path

sys.path.insert(0, "/home/hanning/comet/scripts/passorder_search")
from measure_lib import compile_baseline, compile_baseline_dump  # noqa: E402

COMET_ROOT = Path("/home/hanning/comet")
sys.path.insert(0, str(COMET_ROOT))
from src.polybench_paths import find_polybench_utilities  # noqa: E402
from src.correctness import detect_correctness_mode        # noqa: E402


def main():
    program_rel, scratch_dir = sys.argv[1], Path(sys.argv[2])
    pin_cpu = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
    program_path = COMET_ROOT / program_rel
    scratch_dir.mkdir(parents=True, exist_ok=True)

    utils = find_polybench_utilities(str(program_path))
    if utils is None:
        sys.exit(f"could not resolve utilities/ dir for {program_path}")
    source_dir = program_path.parent

    for f in source_dir.iterdir():
        if f.is_file():
            dest_name = "kernel.c" if f.name == program_path.name else f.name
            shutil.copy2(f, scratch_dir / dest_name)

    # ref_bin (POLYBENCH_TIME) is for TIMING only -- its stdout is just the
    # elapsed time, not the computed result, so it must never be used for
    # correctness comparison (see measure_lib.compile_baseline's docstring).
    # ref_bin_dump (POLYBENCH_DUMP_ARRAYS) is the one whose stdout is the
    # real computed array contents, used for detect_correctness_mode() and
    # every correctness_check() call in run_one.py.
    ref_bin = scratch_dir / "ref_bin"
    ok, ms, err = compile_baseline(str(scratch_dir / "kernel.c"), str(utils),
                                    str(scratch_dir), str(ref_bin), runs=3,
                                    pin_cpu=pin_cpu)
    if not ok:
        sys.exit(f"baseline compile/time failed: {err}")

    ref_bin_dump = scratch_dir / "ref_bin_dump"
    ok, err = compile_baseline_dump(str(scratch_dir / "kernel.c"), str(utils),
                                    str(scratch_dir), str(ref_bin_dump))
    if not ok:
        sys.exit(f"baseline DUMP_ARRAYS compile failed: {err}")

    mode = detect_correctness_mode(str(ref_bin_dump))

    (scratch_dir / "baseline.json").write_text(json.dumps({
        "program": program_rel,
        "utils": str(utils),
        "source_dir": str(scratch_dir),
        "baseline_ms": ms,
        "correctness_mode": mode,
        "ref_bin": str(ref_bin),
        "ref_bin_dump": str(ref_bin_dump),
        "pin_cpu": pin_cpu,
    }, indent=1))
    print(f"prepared {scratch_dir}: baseline={ms:.3f}ms mode={mode} utils={utils}")


if __name__ == "__main__":
    main()
