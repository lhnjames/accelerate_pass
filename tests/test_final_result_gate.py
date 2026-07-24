"""Tests for decide_final_result() -- best-observed, no-rollback policy.

Policy (set 2026-07-24 by the project owner):
  * No speed rollback.
  * Reported number = the BEST observed paired speedup across the n confirmation
    runs. If even one run shows a gain, that gain is taken as achievable; only a
    candidate that regresses in EVERY run is reported below 1.0.
  * `significant_gain` = the MEDIAN was also > 1.0 (reliably, not just
    occasionally, faster). `n_positive`/`n_runs` expose how broad the gain was.
  * Correctness is enforced upstream and is unaffected.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from optimize import decide_final_result


def _conf(ratios):
    import statistics
    s = sorted(ratios)
    n = len(s)
    return {"ok": True, "n": n,
            "confirmed_speedup": statistics.median(ratios),
            "best_speedup": max(ratios),
            "n_positive": sum(1 for r in ratios if r > 1.0),
            "speedup_iqr": [s[n // 4], s[(3 * n) // 4] if n > 1 else s[0]]}


class TestBestObserved(unittest.TestCase):
    def test_one_positive_run_is_taken_as_the_result(self):
        # The exact request: mixed runs, at least one positive -> report the
        # positive (best) value, not the median.
        d = decide_final_result(_conf([0.98, 0.99, 1.06, 1.00, 0.995]),
                                has_flags=True, has_source=False, best_speedup=1.1)
        self.assertEqual(d["final_status"], "confirmed")
        self.assertAlmostEqual(d["final_speedup"], 1.06)   # the best, not median
        self.assertEqual(d["n_positive"], 1)
        self.assertFalse(d["significant_gain"])            # median < 1.0

    def test_reliable_gain_flagged_significant(self):
        d = decide_final_result(_conf([1.04, 1.06, 1.07, 1.05, 1.08]),
                                has_flags=True, has_source=False, best_speedup=1.1)
        self.assertAlmostEqual(d["final_speedup"], 1.08)
        self.assertTrue(d["significant_gain"])             # median > 1.0
        self.assertEqual(d["n_positive"], 5)

    def test_universally_bad_reported_below_one(self):
        # No run positive -> the candidate really is a regression; best (still
        # <1.0) is reported honestly, not clamped.
        d = decide_final_result(_conf([0.95, 0.96, 0.97, 0.94, 0.98]),
                                has_flags=True, has_source=False, best_speedup=1.02)
        self.assertEqual(d["n_positive"], 0)
        self.assertAlmostEqual(d["final_speedup"], 0.98)   # least-bad, <1.0
        self.assertFalse(d["significant_gain"])

    def test_exploratory_peak_kept_separate(self):
        d = decide_final_result(_conf([1.01, 1.02, 1.03]),
                                has_flags=True, has_source=False, best_speedup=1.31)
        self.assertAlmostEqual(d["exploratory_speedup"], 1.31)
        self.assertAlmostEqual(d["final_speedup"], 1.03)   # best confirmed, not the peak


class TestUnconfirmedAndBaseline(unittest.TestCase):
    def test_failed_confirmation_uses_exploration(self):
        d = decide_final_result({"ok": False}, has_flags=True, has_source=False,
                                best_speedup=1.21)
        self.assertEqual(d["final_status"], "exploratory_only")
        self.assertAlmostEqual(d["final_speedup"], 1.21)

    def test_no_candidate_is_baseline(self):
        d = decide_final_result({"ok": False}, has_flags=False, has_source=False,
                                best_speedup=1.0)
        self.assertEqual(d["final_status"], "baseline_only")
        self.assertEqual(d["final_speedup"], 1.0)


if __name__ == "__main__":
    unittest.main()
