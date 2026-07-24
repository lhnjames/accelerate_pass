import math
import unittest

from src.experiment_stats import bootstrap_median_ci, summarize_paired_performance


class ExperimentStatsTests(unittest.TestCase):
    def test_paired_summary_keeps_raw_samples_and_uses_pairwise_ratios(self):
        summary = summarize_paired_performance(
            [4.0, 6.0, 8.0], [2.0, 3.0, 8.0],
            bootstrap_seed=101, bootstrap_resamples=200)
        self.assertEqual(summary.baseline_seconds, (4.0, 6.0, 8.0))
        self.assertEqual(summary.candidate_seconds, (2.0, 3.0, 8.0))
        self.assertEqual(summary.paired_speedups, (2.0, 2.0, 1.0))
        self.assertEqual(summary.median_paired_speedup, 2.0)
        self.assertEqual(summary.sample_count, 3)
        self.assertTrue(math.isfinite(summary.speedup_cv))

    def test_bootstrap_is_seeded_and_deterministic(self):
        values = [0.99, 1.01, 1.04, 1.08, 1.11]
        first = bootstrap_median_ci(values, resamples=500, seed=211)
        second = bootstrap_median_ci(values, resamples=500, seed=211)
        self.assertEqual(first, second)
        self.assertLessEqual(first[0], 1.04)
        self.assertGreaterEqual(first[1], 1.04)

    def test_single_pair_has_zero_cv_and_point_interval(self):
        summary = summarize_paired_performance(
            [2.0], [1.0], bootstrap_resamples=20)
        self.assertEqual(summary.speedup_cv, 0.0)
        self.assertEqual(summary.bootstrap_ci95, (2.0, 2.0))

    def test_rejects_invalid_or_unpaired_samples(self):
        with self.assertRaises(ValueError):
            summarize_paired_performance([], [])
        with self.assertRaises(ValueError):
            summarize_paired_performance([1.0], [1.0, 2.0])
        with self.assertRaises(ValueError):
            summarize_paired_performance([1.0], [0.0])
        with self.assertRaises(ValueError):
            summarize_paired_performance([float("nan")], [1.0])


if __name__ == "__main__":
    unittest.main()
