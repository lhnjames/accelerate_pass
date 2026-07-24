"""Run OpenEvolve on each PolyBench kernel and score its winner exactly as COMET.

For every program:
  1. Run `openevolve-run.py initial_program.c evaluator.py --config config.yaml
     --iterations R` (R matched to COMET's --rounds).
  2. Take OpenEvolve's best evolved kernel.
  3. Re-measure it with COMET's OWN final-confirmation code path:
       - golden correctness (full -DPOLYBENCH_DUMP_ARRAYS array comparison vs the
         untouched reference), and
       - confirm_result_external() alternating baseline/candidate at n=runs,
         then decide_final_result()'s reporting gate (confirmed<1.0 -> rollback
         to 1.0, exactly like COMET).
  4. Append a row in the SAME unified schema the ablation uses, so a single
     summarizer can put COMET and OpenEvolve side by side.

Step 3 is the crux of fairness: OpenEvolve's in-loop fitness and COMET's in-loop
screening differ, but BOTH systems' FINAL reported numbers are produced by this
one identical measurement+gate, so the comparison comes down to search quality,
not measurement bookkeeping.

Run this ONLY when nothing else is timing on the host (never alongside the
ablation sweep) -- shared memory bandwidth would corrupt both sides' timings.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT))

from optimize import confirm_result_external, decide_final_result


def _compile(clang, src_c, util_dir, src_inc, polybench_c, out_bin,
             defines, timeout=600):
    cmd = [clang, "-O3", "-I", util_dir, "-I", src_inc, *defines,
           src_c, polybench_c, "-o", str(out_bin), "-lm"]
    p = subprocess.run(cmd, capture_output=True, timeout=timeout)
    return p.returncode == 0, p.stderr.decode("utf-8", "replace")


def _nums(text):
    return [float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)]


def _golden_correctness(clang, ref_c, cand_c, util_dir, src_inc, polybench_c,
                        pin_cpu, dataset, tol, wd):
    """Independent DUMP_ARRAYS comparison; returns (ok, max_rel_err, note)."""
    ref_bin, cand_bin = wd / "gc_ref", wd / "gc_cand"
    defs = [f"-D{dataset}", "-DPOLYBENCH_DUMP_ARRAYS"]
    ok_r, er = _compile(clang, ref_c, util_dir, src_inc, polybench_c, ref_bin, defs)
    ok_c, ec = _compile(clang, cand_c, util_dir, src_inc, polybench_c, cand_bin, defs)
    if not ok_c:
        return False, None, f"cand compile failed: {ec[:150]}"
    if not ok_r:
        return False, None, f"ref compile failed: {er[:150]}"
    pin = ["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []
    ref_out = _nums(subprocess.run(pin + [str(ref_bin)], capture_output=True,
                                   timeout=600).stderr.decode("utf-8", "replace"))
    cand_out = _nums(subprocess.run(pin + [str(cand_bin)], capture_output=True,
                                    timeout=600).stderr.decode("utf-8", "replace"))
    if not ref_out or len(ref_out) != len(cand_out):
        return False, None, f"dump count {len(ref_out)} vs {len(cand_out)}"
    mre = max(abs(a - b) / max(abs(a), 1e-30) for a, b in zip(ref_out, cand_out))
    return (mre <= tol), mre, ("ok" if mre <= tol else "numeric mismatch")


def run_program(stem, pdir: Path, args) -> dict:
    started = time.time()
    cfg = json.loads((pdir / "eval_config.json").read_text())
    clang = cfg["clang"]; util_dir = cfg["utilities_dir"]
    src_inc = cfg["source_include_dir"]; ref_c = cfg["reference_c"]
    polybench_c = cfg["polybench_c"]; pin_cpu = cfg.get("pin_cpu")
    dataset = cfg.get("dataset_define", "LARGE_DATASET")

    oe_out = pdir / "oe_out"
    log_path = pdir / "openevolve_run.log"
    cmd = [args.python, str(args.openevolve_run),
           str(pdir / "initial_program.c"), str(pdir / "evaluator.py"),
           "--config", str(pdir / "config.yaml"),
           "--iterations", str(args.rounds), "--output", str(oe_out)]
    print(f"[{datetime.now(timezone.utc).isoformat(timespec='seconds')}] "
          f"OpenEvolve {stem} (iterations={args.rounds})", flush=True)
    row = {"program": stem, "system": "openevolve", "condition": "openevolve",
           "seed": args.seed, "rounds_budget": args.rounds,
           "runs_per_confirm": args.runs, "status": "ok"}
    try:
        with log_path.open("w") as fh:
            proc = subprocess.run(cmd, cwd=str(PROJECT_ROOT), stdout=fh,
                                  stderr=subprocess.STDOUT, timeout=args.timeout)
        row["openevolve_returncode"] = proc.returncode
    except subprocess.TimeoutExpired:
        row.update(status="timeout", error=f"exceeded {args.timeout}s")
        row["wall_seconds"] = round(time.time() - started, 1)
        return row

    best_c = oe_out / "best" / "best_program.c"
    best_info = oe_out / "best" / "best_program_info.json"
    if not best_c.exists():
        row.update(status="failed", error="no best_program.c produced")
        row["wall_seconds"] = round(time.time() - started, 1)
        return row
    if best_info.exists():
        try:
            info = json.loads(best_info.read_text())
            row["openevolve_metrics"] = info.get("metrics", {})
        except Exception:
            pass

    # ── final measurement: identical to COMET ──────────────────────────────
    with tempfile.TemporaryDirectory() as td:
        wd = Path(td)
        ok_g, mre, note = _golden_correctness(
            clang, ref_c, str(best_c), util_dir, src_inc, polybench_c,
            pin_cpu, dataset, args.rel_tol, wd)
        row["golden_correctness"] = bool(ok_g)
        row["golden_max_rel_err"] = mre
        row["golden_note"] = note

        base_bin, best_bin = wd / "confirm_base", wd / "confirm_best"
        tdefs = [f"-D{dataset}", "-DPOLYBENCH_TIME"]
        ok_b, _ = _compile(clang, ref_c, util_dir, src_inc, polybench_c, base_bin, tdefs)
        ok_c2, cerr = _compile(clang, str(best_c), util_dir, src_inc, polybench_c, best_bin, tdefs)

        confirmation = {"ok": False}
        if ok_b and ok_c2:
            confirmation = confirm_result_external(str(base_bin), str(best_bin),
                                                   args.runs, pin_cpu)
        row["confirmation"] = confirmation if confirmation.get("ok") else None

        # If the evolved winner is numerically WRONG, it is not a result at all
        # -- force baseline, regardless of how fast it timed. (COMET's gate keys
        #  off confirmed<1.0; here we additionally hard-reject on correctness,
        #  since OpenEvolve's own gate is separate from ours.)
        if not ok_g:
            row.update(final_status="rejected_incorrect", final_speedup=1.0,
                       rollback_reason=f"golden correctness failed: {note}",
                       exploratory_speedup=(row.get("openevolve_metrics", {}) or {}).get("speedup"))
        else:
            has_change = best_c.read_text() != (pdir / "initial_program.c").read_text()
            decision = decide_final_result(
                confirmation, has_flags=False, has_source=has_change,
                best_speedup=(confirmation.get("confirmed_speedup") or 1.0))
            row.update(final_status=decision["final_status"],
                       final_speedup=decision["final_speedup"],
                       rollback_reason=decision["rollback_reason"],
                       exploratory_speedup=decision["exploratory_speedup"])
            row["confirmed_speedup"] = (confirmation.get("confirmed_speedup")
                                        if confirmation.get("ok") else None)
            row["baseline_ms"] = confirmation.get("base_median_ms") if confirmation.get("ok") else None
            if confirmation.get("ok"):
                iqr = confirmation.get("speedup_iqr")
                row["confirm_iqr"] = iqr
                row["iqr_excludes_one"] = (iqr[0] > 1.0) if iqr else None
                row["confirm_n"] = confirmation.get("n")

    row["wall_seconds"] = round(time.time() - started, 1)
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--programs-root",
                    default=str(PROJECT_ROOT / "openevolve_compare" / "programs"))
    ap.add_argument("--openevolve-run", required=True,
                    help="path to openevolve-run.py")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--rounds", type=int, default=3, help="== COMET --rounds")
    ap.add_argument("--runs", type=int, default=5,
                    help="final confirmation alternating repeats (>=3)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rel-tol", type=float, default=1e-6)
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--results", default=str(PROJECT_ROOT / "openevolve_compare" / "results.jsonl"))
    args = ap.parse_args()

    if args.runs < 3:
        ap.error("--runs must be >= 3 (protocol: final confirmation alternates >=3x)")

    root = Path(args.programs_root)
    results_path = Path(args.results)
    results_path.parent.mkdir(parents=True, exist_ok=True)

    done = set()
    if results_path.exists():
        for line in results_path.read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                if r.get("status") in ("ok", "timeout"):
                    done.add(r["program"])

    dirs = sorted(p for p in root.iterdir() if p.is_dir()
                  and (args.only is None or p.name in args.only))
    todo = [p for p in dirs if p.name not in done]
    print(f"{len(dirs)} programs, {len(done)} done, {len(todo)} to run", flush=True)

    for i, pdir in enumerate(todo, 1):
        print(f"--- {i}/{len(todo)}: {pdir.name} ---", flush=True)
        try:
            row = run_program(pdir.name, pdir, args)
        except Exception as exc:
            row = {"program": pdir.name, "system": "openevolve",
                   "status": "error", "error": repr(exc)}
        with results_path.open("a") as fh:
            fh.write(json.dumps(row, ensure_ascii=False) + "\n")
        print(f"    -> {row.get('status')} final={row.get('final_speedup')} "
              f"({row.get('final_status')}) correct={row.get('golden_correctness')} "
              f"{row.get('wall_seconds')}s", flush=True)

    print(f"done; results at {results_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
