#!/usr/bin/env python3
"""Aggregate independent search repetitions without hiding failed trials."""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

from src.experiment_stats import bootstrap_median_ci


def summarize(payload: dict, *, bootstrap_seed: int = 0,
              bootstrap_resamples: int = 10_000) -> dict:
    summary = {
        "schema_version": 1,
        "catalog_size": payload.get("catalog_size"),
        "methods": {},
    }
    for method, repetitions in (payload.get("methods") or {}).items():
        best_speedups = []
        failed = 0
        evaluations = 0
        records = []
        for repetition in repetitions:
            trials = repetition.get("trials", [])
            evaluations += len(trials)
            failed += sum(not trial.get("success", True) for trial in trials)
            successful = [trial for trial in trials if trial.get("success", True)]
            record = {
                "seed": repetition.get("seed"),
                "failed_trials": sum(
                    not trial.get("success", True) for trial in trials),
                "evaluations": len(trials),
            }
            if successful:
                best = max(successful, key=lambda trial: trial["objective"])
                best_speedups.append(float(best["objective"]))
                record["best_speedup"] = float(best["objective"])
            else:
                record["best_speedup"] = None
                record["status"] = "no_successful_trial"
            records.append(record)
        method_summary = {
            "repetitions": records,
            "successful_repetitions": len(best_speedups),
            "failed_trials": failed,
            "evaluations": evaluations,
        }
        if best_speedups:
            method_summary.update({
                "median_best_speedup": statistics.median(best_speedups),
                "best_speedup_iqr": (
                    statistics.quantiles(best_speedups, n=4, method="inclusive")[2]
                    - statistics.quantiles(best_speedups, n=4, method="inclusive")[0]
                    if len(best_speedups) > 1 else 0.0),
                "success_probability_gt_1_01": (
                    sum(value > 1.01 for value in best_speedups) / len(best_speedups)),
                "bootstrap_ci95": bootstrap_median_ci(
                    best_speedups, seed=bootstrap_seed,
                    resamples=bootstrap_resamples),
            })
        summary["methods"][method] = method_summary
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bootstrap-seed", type=int, default=0)
    parser.add_argument("--bootstrap-resamples", type=int, default=10_000)
    args = parser.parse_args()
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    output = summarize(payload, bootstrap_seed=args.bootstrap_seed,
                       bootstrap_resamples=args.bootstrap_resamples)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
