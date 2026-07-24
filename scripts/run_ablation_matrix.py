#!/usr/bin/env python3
"""Drive the Full vs No-compiler-feedback ablation over a program list.

Both conditions are matched on everything except the feedback channel: same
programs, same host/CPU pinning, same compiler, same rounds/runs budget, same
model, same confirmation repeat count.  Only --no-compiler-feedback differs.

Cells are ordered SEED-MAJOR (every program x both conditions for seed 1, then
seed 2, ...) so that an interrupted sweep still yields a complete, balanced
comparison for the seeds that did finish, rather than a full sweep of one
condition and none of the other.

Each finished cell is appended to results.jsonl immediately, so a crash or a
kill never loses completed work, and re-running skips what is already there.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]

# All 30 PolyBench/C 4.2 kernels.  The 9 memory-bound / hard-to-vectorize
# "priority" kernels from the handoff are listed FIRST (they were the initial
# sweep and already have seed-1 results that resume will skip); the remaining
# 21 follow.  Nussinov.orig.c is a reference copy, not a benchmark, and is
# excluded.
DEFAULT_PROGRAMS = [
    # -- priority 9 (already swept at seed 1) --
    "PolyBenchC_no_rag/linear-algebra/kernels/3mm/3mm.c",
    "PolyBenchC_no_rag/medley/nussinov/nussinov.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c",
    "PolyBenchC_no_rag/medley/floyd-warshall/floyd-warshall.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/gramschmidt/gramschmidt.c",
    "PolyBenchC_no_rag/datamining/covariance/covariance.c",
    "PolyBenchC_no_rag/datamining/correlation/correlation.c",
    "PolyBenchC_no_rag/stencils/adi/adi.c",
    "PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c",
    # -- remaining 21 --
    "PolyBenchC_no_rag/linear-algebra/blas/gemm/gemm.c",
    "PolyBenchC_no_rag/linear-algebra/blas/gemver/gemver.c",
    "PolyBenchC_no_rag/linear-algebra/blas/gesummv/gesummv.c",
    "PolyBenchC_no_rag/linear-algebra/blas/symm/symm.c",
    "PolyBenchC_no_rag/linear-algebra/blas/syr2k/syr2k.c",
    "PolyBenchC_no_rag/linear-algebra/blas/syrk/syrk.c",
    "PolyBenchC_no_rag/linear-algebra/blas/trmm/trmm.c",
    "PolyBenchC_no_rag/linear-algebra/kernels/2mm/2mm.c",
    "PolyBenchC_no_rag/linear-algebra/kernels/atax/atax.c",
    "PolyBenchC_no_rag/linear-algebra/kernels/bicg/bicg.c",
    "PolyBenchC_no_rag/linear-algebra/kernels/doitgen/doitgen.c",
    "PolyBenchC_no_rag/linear-algebra/kernels/mvt/mvt.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/durbin/durbin.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/ludcmp/ludcmp.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c",
    "PolyBenchC_no_rag/linear-algebra/solvers/trisolv/trisolv.c",
    "PolyBenchC_no_rag/medley/deriche/deriche.c",
    "PolyBenchC_no_rag/stencils/fdtd-2d/fdtd-2d.c",
    "PolyBenchC_no_rag/stencils/heat-3d/heat-3d.c",
    "PolyBenchC_no_rag/stencils/jacobi-1d/jacobi-1d.c",
    "PolyBenchC_no_rag/stencils/jacobi-2d/jacobi-2d.c",
]

CONDITIONS = ("full", "no_feedback")


def _cell_key(program: str, condition: str, seed: int) -> str:
    return f"{Path(program).stem}|{condition}|{seed}"


def _load_done(results_path: Path) -> set[str]:
    done = set()
    if results_path.exists():
        for line in results_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            # Terminal states that resume must NOT re-run:
            #   ok      -- succeeded.
            #   timeout -- deterministic budget exhaustion. Re-running under the
            #              SAME per-cell budget just burns another full timeout
            #              (observed: cholesky/no_feedback explores until the 2h
            #              ceiling every time). It is already recorded as a
            #              matched-budget non-result; retrying changes nothing.
            # A "failed"/"error" cell, by contrast, may have hit a transient
            # cause (a malformed LLM response, a blip) and IS retried.
            if row.get("status") in ("ok", "timeout"):
                done.add(_cell_key(row["program_path"], row["condition"],
                                   row["seed"]))
    return done


def _find_result_json(program: str, run_dir: str) -> Path | None:
    """Locate <name>_agent_results.json -- the single source of truth for the
    unified ablation schema (it carries condition/seed/final_status/
    rollback_reason after the reporting-gate fix).

    When the run logger is active (the normal case) optimize.py writes it under
    the PER-RUN directory, e.g.
        runs/2026-07-22_18-11-36_polybench_3mm/outputs/3mm_agent_results.json
    and NOT under the project-level outputs/.  Prefer the run directory, which
    is also the only copy that cannot be overwritten by the next cell for the
    same program; fall back to the project-level path for --no-log runs.
    """
    name = Path(program).stem
    candidates = []
    if run_dir:
        candidates.append(Path(run_dir) / "outputs" / f"{name}_agent_results.json")
    candidates.append(PROJECT_ROOT / "outputs" / f"{name}_agent_results.json")
    return next((c for c in candidates if c.exists()), None)


def _latest_run_dir(program: str, since: float) -> str:
    """Absolute path of the runs/ directory this cell produced (for the report)."""
    runs = PROJECT_ROOT / "runs"
    if not runs.is_dir():
        return ""
    name = Path(program).stem
    best, best_mtime = "", since
    for entry in runs.iterdir():
        if entry.is_dir() and entry.name.endswith(f"_{name}"):
            mtime = entry.stat().st_mtime
            if mtime >= best_mtime:
                best, best_mtime = str(entry.resolve()), mtime
    return best


def run_cell(program: str, condition: str, seed: int, args) -> dict:
    started = time.time()
    cmd = [
        args.python, "-u", str(PROJECT_ROOT / "optimize.py"),
        "--program", program,
        "--rounds", str(args.rounds),
        "--runs", str(args.runs),
        "--dataset", args.dataset,
        "--seed", str(seed),
    ]
    if args.pin_cpu is not None:
        cmd += ["--pin-cpu", str(args.pin_cpu)]
    if args.quick_check:
        cmd.append("--quick-check")
    if condition == "no_feedback":
        cmd.append("--no-compiler-feedback")

    log_dir = PROJECT_ROOT / args.log_dir
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{Path(program).stem}_{condition}_seed{seed}.log"

    print(f"[{datetime.now(timezone.utc).isoformat(timespec='seconds')}] "
          f"RUN {Path(program).stem} / {condition} / seed={seed}", flush=True)
    with log_path.open("w", encoding="utf-8") as handle:
        proc = subprocess.run(cmd, cwd=PROJECT_ROOT, stdout=handle,
                              stderr=subprocess.STDOUT, timeout=args.timeout)

    row = {
        "program": Path(program).stem,
        "program_path": program,
        "condition": condition,
        "seed": seed,
        "returncode": proc.returncode,
        "wall_seconds": round(time.time() - started, 1),
        "log_path": str(log_path.resolve()),
        "run_dir": _latest_run_dir(program, started),
        "rounds_budget": args.rounds,
        "runs_per_confirm": args.runs,
        "quick_check": args.quick_check,
        "status": "ok" if proc.returncode == 0 else "failed",
    }

    result_json = _find_result_json(program, row["run_dir"])
    if proc.returncode == 0 and result_json:
        payload = json.loads(result_json.read_text(encoding="utf-8"))
        # Carry the unified schema straight through -- do not recompute any
        # speedup here; optimize.py's reporting gate already decided what may
        # and may not be published for this cell.
        for field in ("baseline_ms", "final_status", "final_speedup",
                      "confirmed_speedup", "exploratory_speedup",
                      "rollback_reason", "rolled_back_flags",
                      "rolled_back_source", "best_flags", "feedback_used",
                      "steps_taken", "has_source_rewrite"):
            row[field] = payload.get(field)
        row["candidate_count"] = len(payload.get("flags_timeline") or [])
        # Snapshot the per-cell result JSON, which optimize.py overwrites on the
        # next run of the same program.
        archive = log_dir / f"{Path(program).stem}_{condition}_seed{seed}_result.json"
        archive.write_text(json.dumps(payload, indent=2, ensure_ascii=False))
        row["result_json"] = str(archive.resolve())
    elif proc.returncode == 0:
        row["status"] = "failed"
        row["error"] = "optimize.py exited 0 but wrote no agent_results.json"

    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--programs", nargs="*", default=DEFAULT_PROGRAMS)
    parser.add_argument("--programs-file", default=None,
                        help="Read program paths from this file (one per line, "
                             "e.g. CBench_shim_root/manifest.txt); overrides --programs.")
    parser.add_argument("--dataset", default="polybench",
                        help="Dataset label passed to optimize.py --dataset "
                             "(polybench|cbench|tsvc|auto).")
    parser.add_argument("--conditions", nargs="*", default=list(CONDITIONS))
    parser.add_argument("--seeds", type=int, nargs="*", default=[1, 2, 3])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--runs", type=int, default=5,
                        help="alternating baseline/candidate repeats used by the "
                             "final confirmation (>=3 required by the protocol)")
    parser.add_argument("--pin-cpu", type=int, default=2)
    parser.add_argument("--quick-check", action="store_true")
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--log-dir", default="ablation_logs")
    parser.add_argument("--results", default="ablation_logs/results.jsonl")
    args = parser.parse_args()

    if args.runs < 3:
        parser.error("--runs must be >= 3: the protocol requires the final "
                     "confirmation to alternate baseline/candidate at least 3 times")

    programs = args.programs
    if args.programs_file:
        pf = Path(args.programs_file)
        if not pf.is_absolute():
            pf = PROJECT_ROOT / pf
        programs = [ln.strip() for ln in pf.read_text().splitlines() if ln.strip()]
        print(f"loaded {len(programs)} programs from {pf}", flush=True)

    results_path = PROJECT_ROOT / args.results
    results_path.parent.mkdir(parents=True, exist_ok=True)
    done = _load_done(results_path)

    cells = [(p, c, s) for s in args.seeds
             for p in programs for c in args.conditions]
    todo = [cell for cell in cells if _cell_key(*cell) not in done]
    print(f"{len(cells)} cells total, {len(done)} already done, "
          f"{len(todo)} to run", flush=True)

    for index, (program, condition, seed) in enumerate(todo, 1):
        print(f"--- cell {index}/{len(todo)} ---", flush=True)
        try:
            row = run_cell(program, condition, seed, args)
        except subprocess.TimeoutExpired:
            row = {"program": Path(program).stem, "program_path": program,
                   "condition": condition, "seed": seed, "status": "timeout",
                   "error": f"exceeded {args.timeout}s"}
        except Exception as exc:                      # keep the sweep alive
            row = {"program": Path(program).stem, "program_path": program,
                   "condition": condition, "seed": seed, "status": "error",
                   "error": repr(exc)}
        with results_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
        print(f"    -> {row['status']} "
              f"final={row.get('final_speedup')} "
              f"({row.get('final_status')}) "
              f"{row.get('wall_seconds', '?')}s", flush=True)

    print(f"sweep complete; results at {results_path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
