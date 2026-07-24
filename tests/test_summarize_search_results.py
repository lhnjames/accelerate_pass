import unittest

from scripts.summarize_search_results import summarize


class SearchSummaryTests(unittest.TestCase):
    def test_aggregates_success_probability_and_failed_trials(self):
        payload = {
            "catalog_size": 4,
            "methods": {
                "random": [
                    {"seed": 101, "trials": [
                        {"objective": 1.02, "success": True},
                        {"objective": -1e12, "success": False},
                    ]},
                    {"seed": 211, "trials": [
                        {"objective": 0.99, "success": True},
                    ]},
                ]
            }
        }
        result = summarize(payload, bootstrap_resamples=100, bootstrap_seed=211)
        random = result["methods"]["random"]
        self.assertEqual(random["failed_trials"], 1)
        self.assertEqual(random["evaluations"], 3)
        self.assertEqual(random["successful_repetitions"], 2)
        self.assertEqual(random["success_probability_gt_1_01"], 0.5)
        self.assertEqual(len(random["bootstrap_ci95"]), 2)


if __name__ == "__main__":
    unittest.main()
