"""Tests for decide_final_result() -- median-of-confirmation, no-rollback policy.

Policy (median reporting set 2026-08-02, replacing best-observed reporting):
  * No speed rollback.
  * Reported number = the MEDIAN paired speedup across the n confirmation runs.
    The maximum is kept as `best_observed_speedup` but is no longer the
    headline: taking the max of n noisy samples biases the estimate up by
    ~0.85 stdev at n=3 by construction, which inflated this study's condition-1
    geomean by 9.3% and its sub-10ms cBench subset by 22%.
  * `significant_gain` requires the whole IQR to sit above 1.0 AND every paired
    run to be a gain -- median > 1.0 alone let telecom_crc32 be flagged
    significant on an IQR of [0.853, 1.635], an interval that does not even
    determine the sign of the effect.
  * Correctness is enforced upstream and is unaffected.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from optimize import decide_final_result, _adaptive_confirm_runs


def _conf(ratios):
    import statistics
    s = sorted(ratios)
    n = len(s)
    return {"ok": True, "n": n,
            "confirmed_speedup": statistics.median(ratios),
            "best_speedup": max(ratios),
            "n_positive": sum(1 for r in ratios if r > 1.0),
            "speedup_iqr": [s[n // 4], s[(3 * n) // 4] if n > 1 else s[0]]}


class TestMedianReporting(unittest.TestCase):
    def test_mixed_runs_report_median_not_the_lucky_one(self):
        d = decide_final_result(_conf([0.98, 0.99, 1.06, 1.00, 0.995]),
                                has_flags=True, has_source=False, best_speedup=1.1)
        self.assertEqual(d["final_status"], "confirmed")
        self.assertAlmostEqual(d["final_speedup"], 0.995)          # median
        self.assertAlmostEqual(d["best_observed_speedup"], 1.06)   # max, kept aside
        self.assertEqual(d["n_positive"], 1)
        self.assertFalse(d["significant_gain"])

    def test_reliable_gain_flagged_significant(self):
        d = decide_final_result(_conf([1.04, 1.06, 1.07, 1.05, 1.08]),
                                has_flags=True, has_source=False, best_speedup=1.1)
        self.assertAlmostEqual(d["final_speedup"], 1.06)
        self.assertTrue(d["significant_gain"])   # IQR above 1.0 and 5/5 positive
        self.assertEqual(d["n_positive"], 5)

    def test_universally_bad_reported_below_one(self):
        d = decide_final_result(_conf([0.95, 0.96, 0.97, 0.94, 0.98]),
                                has_flags=True, has_source=False, best_speedup=1.02)
        self.assertEqual(d["n_positive"], 0)
        self.assertAlmostEqual(d["final_speedup"], 0.96)   # median, not least-bad
        self.assertFalse(d["significant_gain"])

    def test_exploratory_peak_kept_separate(self):
        d = decide_final_result(_conf([1.01, 1.02, 1.03]),
                                has_flags=True, has_source=False, best_speedup=1.31)
        self.assertAlmostEqual(d["exploratory_speedup"], 1.31)
        self.assertAlmostEqual(d["final_speedup"], 1.02)

    def test_crc32_regression(self):
        """The measurement that motivated the change.

        telecom_crc32's three confirmation ratios. An independent paired
        re-measurement on an idle core puts the true value at ~1.016x; the old
        policy published 1.6346x and called it significant.
        """
        d = decide_final_result(_conf([0.8535, 1.4852, 1.6346]),
                                has_flags=True, has_source=False, best_speedup=1.461)
        self.assertAlmostEqual(d["final_speedup"], 1.4852)          # no longer 1.6346
        self.assertAlmostEqual(d["best_observed_speedup"], 1.6346)
        self.assertFalse(d["significant_gain"])   # IQR spans 1.0, 2/3 positive


class TestAdaptiveConfirmRuns(unittest.TestCase):
    def test_short_benchmarks_get_many_more_samples(self):
        # 0.8 ms binary: 51 runs costs 40 ms and actually pins down the median.
        self.assertEqual(_adaptive_confirm_runs(3, 0.8), 51)

    def test_long_benchmarks_keep_the_requested_count(self):
        # A 108-second PolyBench kernel must not be sampled 51 times.
        self.assertEqual(_adaptive_confirm_runs(3, 108000.0), 3)

    def test_never_below_the_request(self):
        self.assertEqual(_adaptive_confirm_runs(9, 100000.0), 9)

    def test_count_is_odd_so_the_median_is_a_real_sample(self):
        for per_run in (0.5, 3.0, 27.6, 62.0, 250.0):
            self.assertEqual(_adaptive_confirm_runs(3, per_run) % 2, 1)

    def test_degenerate_timing_falls_back_to_request(self):
        self.assertEqual(_adaptive_confirm_runs(3, 0.0), 3)


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
