"""Put COMET (full / no_feedback) and OpenEvolve side by side, per program.

Reads the ablation sweep's results.jsonl (COMET) and the OpenEvolve compare
results.jsonl, both in the same unified schema, and emits a per-program table
plus per-system geomeans and a paired COMET-vs-OpenEvolve comparison over the
programs both systems finished.

All three systems' `final_speedup` are produced by the SAME confirmation code
path (confirm_result_external + decide_final_result) with the same n and the
same golden correctness discipline, so the numbers are directly comparable.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT))
from src.experiment_stats import bootstrap_median_ci


def _geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(statistics.fmean(math.log(x) for x in xs)) if xs else float("nan")


def _load(path):
    rows = []
    p = Path(path)
    if p.exists():
        for line in p.read_text().splitlines():
            if line.strip():
                rows.append(json.loads(line))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ablation", default=str(PROJECT_ROOT / "ablation_logs" / "results.jsonl"))
    ap.add_argument("--openevolve", default=str(PROJECT_ROOT / "openevolve_compare" / "results.jsonl"))
    ap.add_argument("--seed", type=int, default=1, help="COMET seed to use")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    comet = _load(args.ablation)
    oe = _load(args.openevolve)

    # program -> system -> final_speedup (COMET filtered to the chosen seed)
    table: dict[str, dict] = defaultdict(dict)
    for r in comet:
        if r.get("status") != "ok" or r.get("seed") != args.seed:
            continue
        if isinstance(r.get("final_speedup"), (int, float)):
            table[r["program"]][f"comet_{r['condition']}"] = r["final_speedup"]
    for r in oe:
        if r.get("status") != "ok":
            continue
        if isinstance(r.get("final_speedup"), (int, float)):
            table[r["program"]]["openevolve"] = r["final_speedup"]
            table[r["program"]]["_oe_correct"] = r.get("golden_correctness")

    systems = ["comet_full", "comet_no_feedback", "openevolve"]
    per_system_vals = defaultdict(list)
    rows_out = []
    for prog in sorted(table):
        cell = table[prog]
        rows_out.append({"program": prog,
                         **{s: cell.get(s) for s in systems},
                         "openevolve_correct": cell.get("_oe_correct")})
        for s in systems:
            if cell.get(s):
                per_system_vals[s].append(cell[s])

    summary = {system: {
        "programs": len(per_system_vals[system]),
        "geomean": _geomean(per_system_vals[system]),
        "success_rate_ge_1": (sum(1 for v in per_system_vals[system] if v >= 1.0)
                              / len(per_system_vals[system])) if per_system_vals[system] else None,
        "strict_gain_gt_1_01": (sum(1 for v in per_system_vals[system] if v > 1.01)
                                / len(per_system_vals[system])) if per_system_vals[system] else None,
    } for system in systems}

    # paired: COMET-full vs OpenEvolve on programs both finished
    def paired(a, b):
        pairs = [(table[p][a], table[p][b]) for p in table
                 if table[p].get(a) and table[p].get(b)]
        if not pairs:
            return {"paired": 0}
        ratios = [x / y for x, y in pairs]
        return {
            "paired": len(pairs),
            f"geomean_{a}": _geomean([x for x, _ in pairs]),
            f"geomean_{b}": _geomean([y for _, y in pairs]),
            "geomean_ratio": _geomean(ratios),
            "ci95_ratio": (list(bootstrap_median_ci(ratios, seed=0))
                           if len(ratios) > 1 else [ratios[0], ratios[0]]),
            f"{a}_wins": sum(1 for x, y in pairs if x > y + 1e-9),
            f"{b}_wins": sum(1 for x, y in pairs if y > x + 1e-9),
            "ties": sum(1 for x, y in pairs if abs(x - y) <= 1e-9),
        }

    report = {
        "per_system": summary,
        "paired_comet_full_vs_openevolve": paired("comet_full", "openevolve"),
        "paired_comet_no_feedback_vs_openevolve": paired("comet_no_feedback", "openevolve"),
        "table": rows_out,
    }
    text = json.dumps(report, indent=2, ensure_ascii=False)
    print(text)
    if args.json_out:
        Path(args.json_out).write_text(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
