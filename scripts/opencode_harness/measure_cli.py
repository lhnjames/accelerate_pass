#!/usr/bin/env python3
"""CLI opencode calls itself (via ./measure.sh) to check its own progress.
Single-run timing (like comet's own _screen_runs=1 fast-feedback screening)
-- cheap, noisy, just for the agent's own iteration; the harness does the
real confirmed measurement separately once opencode is done.

Usage: measure_cli.py <utils_dir> <source_dir> <baseline_ms> [pin_cpu]
Run from inside the scratch dir; compiles ./kernel.c.
"""
import sys
from pathlib import Path
sys.path.insert(0, "/home/hanning/comet/scripts/opencode_harness")
from measure_lib import compile_and_time  # noqa: E402

def main():
    utils, source_dir, baseline_ms = sys.argv[1], sys.argv[2], float(sys.argv[3])
    pin_cpu = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4] else None
    # out_bin lives INSIDE this task's own scratch dir (source_dir), not a
    # shared /tmp path -- parallel opencode workers on the same node would
    # otherwise clobber each other's binary mid-compile/mid-run.
    out_bin = str(Path(source_dir) / "oc_measure_bin")
    ok, ms, err = compile_and_time("kernel.c", utils, source_dir, runs=1,
                                    out_bin=out_bin, pin_cpu=pin_cpu)
    if not ok:
        print(f"COMPILE_OR_RUN_FAILED: {err[:500]}")
        sys.exit(1)
    speedup = baseline_ms / ms if ms > 0 else 0.0
    print(f"OK: {ms:.3f} ms  (baseline {baseline_ms:.3f} ms)  speedup={speedup:.4f}x")

if __name__ == "__main__":
    main()
