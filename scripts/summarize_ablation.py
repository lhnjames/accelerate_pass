#!/usr/bin/env python3
"""Aggregate the ablation sweep into the paper's per-condition statistics.

Consumes the results.jsonl written by run_ablation_matrix.py and emits, per
condition: per-program median speedup, cross-program geometric mean, the
>=1.0 success rate, the >1.01 strict-gain rate, the failure/rollback counts,
and a bootstrap 95% CI.  When both conditions share a program+seed cell, it
also reports the PAIRED Full-vs-No-feedback comparison on exactly those cells.

Every speedup used here is `final_speedup` -- the value optimize.py's reporting
gate certified.  Exploratory single-shot peaks are carried through the table
for transparency but are never aggregated into any published statistic.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from src.experiment_stats import bootstrap_median_ci


def _geomean(values):
    if not values:
        return float("nan")
    return math.exp(statistics.fmean(math.log(v) for v in values))


def load_rows(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            rows.append(_attach_confirmation_spread(json.loads(line)))
    return rows


def _attach_confirmation_spread(row: dict) -> dict:
    """Pull the confirmation IQR / per-side CV out of the archived cell JSON.

    A bare median is not enough to judge a MARGINAL result: a cell reporting
    1.0108x whose confirmation IQR spans 1.0 has not demonstrated a gain at
    all, and must not be counted the same as one whose whole IQR sits above
    1.0.  run_ablation_matrix.py archives the full per-cell result JSON (which
    carries confirm_result_external's IQR and both stdev percentages) and puts
    its path in `result_json`; results.jsonl itself only carries the medians.
    """
    row.setdefault("iqr_low", None)
    row.setdefault("iqr_high", None)
    row.setdefault("iqr_excludes_one", None)
    path = row.get("result_json")
    if not path:
        return row
    try:
        payload = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return row
    confirmation = payload.get("confirmation") or {}
    iqr = confirmation.get("speedup_iqr")
    if isinstance(iqr, (list, tuple)) and len(iqr) == 2:
        row["iqr_low"], row["iqr_high"] = float(iqr[0]), float(iqr[1])
        row["iqr_excludes_one"] = float(iqr[0]) > 1.0
    row["base_stdev_pct"] = confirmation.get("base_stdev_pct")
    row["best_stdev_pct"] = confirmation.get("best_stdev_pct")
    row["confirm_n"] = confirmation.get("n")
    # Best-observed policy transparency fields (present in the per-cell JSON
    # even when the driver's aggregate results.jsonl row predates them).
    for key in ("significant_gain", "confirmed_median", "n_positive", "n_runs"):
        if row.get(key) is None and payload.get(key) is not None:
            row[key] = payload.get(key)
    return row


def summarize_condition(rows: list[dict], *, bootstrap_seed: int = 0) -> dict:
    ok = [r for r in rows if r.get("status") == "ok"
          and isinstance(r.get("final_speedup"), (int, float))]
    if not ok:
        return {"cells": len(rows), "usable_cells": 0}

    by_program = defaultdict(list)
    for row in ok:
        by_program[row["program"]].append(float(row["final_speedup"]))

    # One number per program (median across that program's seeds), then the
    # cross-program geomean -- so a program with more finished seeds cannot
    # dominate the headline figure.
    per_program = {p: statistics.median(v) for p, v in sorted(by_program.items())}
    medians = list(per_program.values())

    speedups = [float(r["final_speedup"]) for r in ok]
    statuses = defaultdict(int)
    for row in rows:
        statuses[row.get("final_status") or row.get("status")] += 1

    return {
        "cells": len(rows),
        "usable_cells": len(ok),
        "programs": len(per_program),
        "seeds_per_program": {p: len(v) for p, v in sorted(by_program.items())},
        "per_program_median_speedup": per_program,
        "geomean_of_program_medians": _geomean(medians),
        "median_cell_speedup": statistics.median(speedups),
        "success_rate_ge_1": sum(1 for s in speedups if s >= 1.0) / len(speedups),
        "strict_gain_rate_gt_1_01": sum(1 for s in speedups if s > 1.01) / len(speedups),
        # The strictest reading: the cell gained AND its confirmation IQR sits
        # entirely above 1.0, so the gain is not an artifact of run-to-run
        # spread.  Cells whose IQR was not recorded are excluded from the
        # numerator, never silently counted as successes.
        "iqr_backed_gain_rate": (
            sum(1 for r in ok if r.get("iqr_excludes_one") is True) / len(ok)),
        "cells_missing_iqr": sum(1 for r in ok if r.get("iqr_excludes_one") is None),
        "bootstrap_ci95_of_program_medians": (
            list(bootstrap_median_ci(medians, seed=bootstrap_seed))
            if len(medians) > 1 else [medians[0], medians[0]]),
        "status_counts": dict(statuses),
        "rollbacks": sum(1 for r in rows
                         if str(r.get("final_status", "")).startswith("rolled_back")),
        "total_wall_seconds": round(sum(r.get("wall_seconds", 0) or 0 for r in rows), 1),
    }


def paired_comparison(rows: list[dict], a: str, b: str,
                      *, bootstrap_seed: int = 0) -> dict:
    """Compare a vs b using ONLY program+seed cells where both finished."""
    index = {}
    for row in rows:
        if row.get("status") == "ok" and isinstance(row.get("final_speedup"), (int, float)):
            index[(row["program"], row["seed"], row["condition"])] = float(row["final_speedup"])

    pairs = []
    for (program, seed, condition), value in sorted(index.items()):
        if condition != a:
            continue
        other = index.get((program, seed, b))
        if other is not None:
            pairs.append({"program": program, "seed": seed, a: value, b: other,
                          "ratio": value / other})
    if not pairs:
        return {"paired_cells": 0,
                "note": f"no program+seed cell finished under BOTH {a} and {b}"}

    ratios = [p["ratio"] for p in pairs]
    wins = sum(1 for p in pairs if p[a] > p[b] + 1e-9)
    ties = sum(1 for p in pairs if abs(p[a] - p[b]) <= 1e-9)
    return {
        "paired_cells": len(pairs),
        "conditions": [a, b],
        f"geomean_{a}": _geomean([p[a] for p in pairs]),
        f"geomean_{b}": _geomean([p[b] for p in pairs]),
        "geomean_ratio_a_over_b": _geomean(ratios),
        "median_ratio_a_over_b": statistics.median(ratios),
        "bootstrap_ci95_of_ratio": (list(bootstrap_median_ci(ratios, seed=bootstrap_seed))
                                    if len(ratios) > 1 else [ratios[0], ratios[0]]),
        f"{a}_wins": wins,
        "ties": ties,
        f"{b}_wins": len(pairs) - wins - ties,
        "pairs": pairs,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", default="ablation_logs/results.jsonl")
    parser.add_argument("--json-out", default="")
    parser.add_argument("--bootstrap-seed", type=int, default=0)
    args = parser.parse_args()

    path = Path(args.results)
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    rows = load_rows(path)

    by_condition = defaultdict(list)
    for row in rows:
        by_condition[row.get("condition", "?")].append(row)

    report = {
        "results_path": str(path.resolve()),
        "total_rows": len(rows),
        "by_condition": {c: summarize_condition(r, bootstrap_seed=args.bootstrap_seed)
                         for c, r in sorted(by_condition.items())},
    }
    if "full" in by_condition and "no_feedback" in by_condition:
        report["paired_full_vs_no_feedback"] = paired_comparison(
            rows, "full", "no_feedback", bootstrap_seed=args.bootstrap_seed)

    # ── unified per-cell table (the schema the handoff asked for) ────────────
    report["cells"] = [{
        "program": r.get("program"),
        "condition": r.get("condition"),
        "seed": r.get("seed"),
        "baseline_ms": r.get("baseline_ms"),
        "best_ms": (round(r["baseline_ms"] / r["final_speedup"], 2)
                    if r.get("baseline_ms") and r.get("final_speedup") else None),
        "confirmed_speedup": r.get("confirmed_speedup"),
        "confirm_n": r.get("confirm_n"),
        "confirm_iqr": ([r.get("iqr_low"), r.get("iqr_high")]
                        if r.get("iqr_low") is not None else None),
        "iqr_excludes_one": r.get("iqr_excludes_one"),
        "final_speedup": r.get("final_speedup"),
        "final_status": r.get("final_status"),
        "exploratory_speedup": r.get("exploratory_speedup"),
        "candidate_count": r.get("candidate_count"),
        "feedback_used": r.get("feedback_used"),
        "rollback_reason": r.get("rollback_reason"),
        "run_dir": r.get("run_dir"),
        "status": r.get("status"),
    } for r in rows]

    text = json.dumps(report, indent=2, ensure_ascii=False)
    print(text)
    if args.json_out:
        out = Path(args.json_out)
        if not out.is_absolute():
            out = PROJECT_ROOT / out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n", encoding="utf-8")
        print(f"\n[written] {out.resolve()}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
