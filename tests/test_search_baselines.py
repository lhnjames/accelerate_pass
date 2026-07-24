import unittest

from src.search_baselines import (
    Candidate, Evaluation, ParameterAxis, bayesian_search,
    build_candidate_catalog, random_search,
)


def catalog():
    return [Candidate((float(x), float(y)), (f"--x={x}", f"--y={y}"))
            for x in range(-3, 4) for y in range(-2, 3)]


def objective(candidate):
    x, y = candidate.values
    return 10.0 - (x - 2.0) ** 2 - (y + 1.0) ** 2


class TestSearchBaselines(unittest.TestCase):
    def test_shared_catalog_is_deterministic_and_formats_mllvm_pairs(self):
        candidates = build_candidate_catalog([
            ParameterAxis("slp-threshold", (-1, 0)),
            ParameterAxis("--unroll-threshold", (100, 200)),
        ])
        self.assertEqual(len(candidates), 4)
        self.assertEqual(candidates[0].values, (-1.0, 100.0))
        self.assertEqual(candidates[0].flags,
                         ("-mllvm", "--slp-threshold=-1",
                          "-mllvm", "--unroll-threshold=100"))
        self.assertEqual(candidates[-1].label,
                         "--slp-threshold=0,--unroll-threshold=200")

    def test_catalog_rejects_unsafe_or_oversized_axes(self):
        with self.assertRaises(ValueError):
            build_candidate_catalog([ParameterAxis("force-vector-width", (1, 2))])
        with self.assertRaises(ValueError):
            build_candidate_catalog([ParameterAxis("x", tuple(range(11)))],
                                    max_candidates=10)

    def test_random_is_seeded_and_consumes_exact_budget(self):
        first = random_search(catalog(), 9, 211, objective)
        second = random_search(catalog(), 9, 211, objective)
        self.assertEqual(
            [(trial.index, trial.objective, trial.success, trial.error)
             for trial in first.trials],
            [(trial.index, trial.objective, trial.success, trial.error)
             for trial in second.trials])
        self.assertEqual(len(first.trials), 9)
        self.assertEqual(len({trial.index for trial in first.trials}), 9)

    def test_bayesian_is_seeded_and_consumes_exact_budget(self):
        first = bayesian_search(catalog(), 12, 307, objective, initial_points=4)
        second = bayesian_search(catalog(), 12, 307, objective, initial_points=4)
        self.assertEqual(
            [(trial.index, trial.objective, trial.success, trial.error)
             for trial in first.trials],
            [(trial.index, trial.objective, trial.success, trial.error)
             for trial in second.trials])
        self.assertEqual(len(first.trials), 12)
        self.assertEqual(len({trial.index for trial in first.trials}), 12)

    def test_budget_is_capped_by_catalog_without_remeasurement(self):
        result = random_search(catalog()[:3], 20, 101, objective)
        self.assertEqual(result.budget, 3)
        self.assertEqual(len(result.trials), 3)

    def test_empty_catalog_and_zero_budget_are_rejected(self):
        with self.assertRaises(ValueError):
            random_search([], 1, 1, objective)
        with self.assertRaises(ValueError):
            bayesian_search(catalog(), 0, 1, objective)

    def test_failed_compile_consumes_budget_and_is_excluded_from_best(self):
        def evaluator(candidate):
            if candidate.values[0] < 0:
                return Evaluation(-1.0, success=False, error="compile failed")
            return objective(candidate)

        result = random_search(catalog(), 9, 211, evaluator)
        self.assertEqual(len(result.trials), 9)
        self.assertEqual(result.failed_count,
                         sum(not trial.success for trial in result.trials))
        self.assertTrue(result.best.success)
        self.assertNotEqual(result.best.error, "compile failed")

    def test_evaluator_exception_is_a_recorded_failed_trial(self):
        calls = []

        def evaluator(candidate):
            calls.append(candidate)
            raise RuntimeError("compiler timeout")

        result = random_search(catalog(), 3, 101, evaluator)
        self.assertEqual(len(calls), 3)
        self.assertEqual(result.failed_count, 3)
        with self.assertRaises(ValueError):
            _ = result.best


if __name__ == "__main__":
    unittest.main()
