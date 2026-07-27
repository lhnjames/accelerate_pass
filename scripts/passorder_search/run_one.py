#!/usr/bin/env python3
"""Drive an explicit N-round pass-order search for one benchmark program,
DeepSeek-guided -- our own approximation of AutoPass's core idea (search
over LLVM pass ORDER) since the actual AutoPass paper/repo was dropped as a
baseline. Same --rounds budget as every other condition in this ablation
study (comet's own agent, OpenCode).

Usage: run_one.py <program_rel_path> <scratch_dir> [rounds=9] [confirm_runs=3] [pin_cpu]
"""
import sys, json, uuid
from pathlib import Path

sys.path.insert(0, "/home/hanning/comet/scripts/passorder_search")
from measure_lib import compile_with_pass_order, time_binary, correctness_check  # noqa: E402
from pass_list import CANONICAL_PASSES  # noqa: E402
from llm_client import ask_json  # noqa: E402

sys.path.insert(0, "/home/hanning/comet")
from optimize import confirm_result_external  # noqa: E402

SYSTEM_PROMPT = f"""You are tuning the ORDER of LLVM optimization passes applied to a single C \
kernel function, on an aarch64 Linux machine (clang/opt/llc LLVM 21). You do NOT edit the \
source code and you do NOT get to pick pass parameters -- only the ORDER (and optionally which \
subset, and optionally repeating a pass) from this fixed catalog of valid `opt -passes=` names:

{", ".join(CANONICAL_PASSES)}

`mem2reg` is always run first automatically -- do not include it yourself.

Rules:
- Respond with ONLY a JSON object: {{"passes": ["name1", "name2", ...], "reasoning": "..."}}
- Every name in "passes" MUST come from the catalog above (repeats and subsets are fine).
- A typical useful order is 15-25 entries; too short under-optimizes, but there is no fixed length.
- You will be told the measured speedup (kernel binary vs -O3 baseline) after each attempt, and \
asked to propose the next order. Use what worked/failed so far to refine -- e.g. if vectorization \
passes (loop-vectorize, slp-vectorizer) come before the cleanup/canonicalization passes they \
depend on (simplifycfg, instcombine, licm, loop-rotate, indvars), moving them later often helps."""


def build_user_prompt(round_num, rounds, history, kernel_name):
    lines = [f"Kernel: {kernel_name}. Round {round_num}/{rounds}."]
    if not history:
        lines.append("This is the first attempt -- propose an initial pass order.")
    else:
        lines.append("History so far (order tried -> result):")
        for h in history[-5:]:
            if h["ok"]:
                lines.append(f"  speedup={h['speedup']:.4f}x  passes={h['passes']}")
            else:
                lines.append(f"  FAILED ({h['error'][:150]})  passes={h['passes']}")
        best = max((h for h in history if h["ok"]), key=lambda h: h["speedup"], default=None)
        if best:
            lines.append(f"Best so far: {best['speedup']:.4f}x with {best['passes']}")
        lines.append("Propose the next pass order to try, aiming to beat the best so far.")
    return "\n".join(lines)


def main():
    program_rel = sys.argv[1]
    scratch_dir = Path(sys.argv[2])
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 9
    confirm_runs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    pin_cpu = int(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None

    baseline = json.loads((scratch_dir / "baseline.json").read_text())
    kernel_name = Path(program_rel).stem
    utils, source_dir = baseline["utils"], baseline["source_dir"]
    baseline_ms = baseline["baseline_ms"]

    work_dir = scratch_dir / "work"
    work_dir.mkdir(exist_ok=True)

    history = []
    best_passes, best_speedup = list(CANONICAL_PASSES), 1.0

    for round_num in range(1, rounds + 1):
        user_prompt = build_user_prompt(round_num, rounds, history, kernel_name)
        reply = ask_json(SYSTEM_PROMPT, user_prompt)
        passes = [p for p in reply.get("passes", []) if p in CANONICAL_PASSES]
        if not passes:
            passes = list(CANONICAL_PASSES)  # fallback: canonical default order

        trial_bin = str(work_dir / f"trial_{round_num}_{uuid.uuid4().hex[:6]}")
        ok, err = compile_with_pass_order(str(scratch_dir / "kernel.c"), utils, source_dir,
                                           passes, trial_bin, work_dir=str(work_dir))
        if not ok:
            history.append({"ok": False, "passes": passes, "error": err})
            print(f"[round {round_num}/{rounds}] FAILED: {err[:200]}")
            continue

        tok, ms = time_binary(trial_bin, runs=1, pin_cpu=pin_cpu)
        if not tok:
            history.append({"ok": False, "passes": passes, "error": "run failed/crashed"})
            print(f"[round {round_num}/{rounds}] run failed")
            continue

        speedup = baseline_ms / ms if ms > 0 else 0.0
        history.append({"ok": True, "passes": passes, "speedup": speedup})
        print(f"[round {round_num}/{rounds}] speedup={speedup:.4f}x  passes={passes}")
        if speedup > best_speedup:
            best_speedup, best_passes = speedup, passes

    (scratch_dir / "history.json").write_text(json.dumps(history, indent=1))
    (scratch_dir / "best.json").write_text(json.dumps(
        {"best_passes": best_passes, "best_speedup": best_speedup}, indent=1))

    # ── Finalize: rebuild the best order into a clean binary, correctness
    # check against ref_bin_dump (POLYBENCH_DUMP_ARRAYS -- real computed
    # output, NOT ref_bin's POLYBENCH_TIME stdout which is only a timer
    # reading; see measure_lib.compile_baseline's docstring), then the SAME
    # alternating-measurement confirmation every other condition uses. ────
    opt_bin = str(scratch_dir / "opt_bin")
    ok, err = compile_with_pass_order(str(scratch_dir / "kernel.c"), utils, source_dir,
                                       best_passes, opt_bin, work_dir=str(work_dir))
    result = {"program": program_rel, "baseline_ms": baseline_ms,
              "best_passes": best_passes, "explored_best_speedup": best_speedup}
    if not ok:
        result.update(status="compile_failed", error=err[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    opt_bin_dump = str(scratch_dir / "opt_bin_dump")
    ok, err = compile_with_pass_order(str(scratch_dir / "kernel.c"), utils, source_dir,
                                       best_passes, opt_bin_dump, work_dir=str(work_dir),
                                       output_macro="POLYBENCH_DUMP_ARRAYS")
    if not ok:
        result.update(status="compile_failed", error=("DUMP_ARRAYS build: " + err)[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    ref_bin_dump = baseline.get("ref_bin_dump", baseline["ref_bin"])
    correct, cerr = correctness_check(ref_bin_dump, opt_bin_dump, baseline["correctness_mode"])
    if not correct:
        result.update(status="incorrect", error=cerr, confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    confirm = confirm_result_external(baseline["ref_bin"], opt_bin, confirm_runs, pin_cpu)
    if not confirm.get("ok"):
        result.update(status="confirm_failed", confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
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
    (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
