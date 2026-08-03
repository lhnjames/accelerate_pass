#!/usr/bin/env python3
"""After opencode has edited kernel.c in a scratch dir, do the SAME rigorous
confirmation measurement + correctness check as conditions (1)/(2)/(3), so
the reported speedup is apples-to-apples.

Usage: finalize.py <scratch_dir> <runs>
"""
import sys, json
from pathlib import Path

sys.path.insert(0, "/home/hanning/comet/scripts/opencode_harness")
from measure_lib import compile_and_time, compile_for_correctness, correctness_check  # noqa: E402

COMET_ROOT = Path("/home/hanning/comet")
sys.path.insert(0, str(COMET_ROOT))
from optimize import confirm_result_external  # noqa: E402


def main():
    scratch_dir, runs = Path(sys.argv[1]), int(sys.argv[2])
    baseline = json.loads((scratch_dir / "baseline.json").read_text())
    pin_cpu = baseline.get("pin_cpu")

    opt_bin = scratch_dir / "opt_bin"
    ok, ms, err = compile_and_time(str(scratch_dir / "kernel.c"), baseline["utils"],
                                    baseline["source_dir"], runs=1, out_bin=str(opt_bin),
                                    pin_cpu=pin_cpu)
    result = {"program": baseline["program"], "baseline_ms": baseline["baseline_ms"]}

    if not ok:
        result.update(status="compile_or_run_failed", error=err[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        return

    # Correctness against the DUMP_ARRAYS pair, never the timing pair. opt_bin
    # and ref_bin are both -DPOLYBENCH_TIME builds that print nothing but their
    # own elapsed time, so checking them against each other compared two
    # stopwatch readings under a 1e-4 relative tolerance -- which is how this
    # harness produced 26 "incorrect" verdicts, 22 of them on tasks whose
    # kernel.c was never edited.
    ref_dump = baseline.get("ref_bin_dump")
    if not ref_dump:
        result.update(status="stale_scratch_dir",
                      error="baseline.json has no ref_bin_dump -- prepared by the "
                            "pre-2026-08-03 harness, which could not check correctness",
                      confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        return
    opt_bin_dump = scratch_dir / "opt_bin_dump"
    ok_d, derr = compile_for_correctness(str(scratch_dir / "kernel.c"), baseline["utils"],
                                          baseline["source_dir"], str(opt_bin_dump))
    if not ok_d:
        result.update(status="compile_or_run_failed", error=derr[:1000],
                      confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        return

    correct, cerr = correctness_check(ref_dump, str(opt_bin_dump),
                                       baseline["correctness_mode"])
    if not correct:
        result.update(status="incorrect", error=cerr,
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        return

    confirm = confirm_result_external(baseline["ref_bin"], str(opt_bin), runs, pin_cpu)
    if not confirm.get("ok"):
        result.update(status="confirm_failed", confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        return

    result.update(
        status="confirmed",
        confirmed_speedup=confirm["confirmed_speedup"],
        best_speedup=confirm["best_speedup"],
        n=confirm["n"],
        n_positive=confirm["n_positive"],
        significant=confirm["confirmed_speedup"] > 1.0,
        speedup_iqr=confirm["speedup_iqr"],
    )
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
