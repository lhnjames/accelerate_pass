import unittest

from scripts.run_search_baseline import (
    _correctness_base_defines, _load_axes, make_parser,
)


class SearchBaselineCliTests(unittest.TestCase):
    def test_parser_keeps_equal_budget_and_correctness_defaults(self):
        args = make_parser().parse_args([
            "--source", "kernel.c", "--axes", "axes.json",
            "--result-dir", "results", "--budget", "12",
            "--seeds", "101", "211", "--pin-cpu", "39",
        ])
        self.assertEqual(args.budget, 12)
        self.assertEqual(args.seeds, [101, 211])
        self.assertEqual(args.pin_cpu, 39)
        self.assertIn("-DPOLYBENCH_DUMP_ARRAYS", args.correctness_define)

    def test_timing_macro_is_excluded_from_correctness_defines(self):
        defines = ["-DKEEP=1", "-DPOLYBENCH_TIME", "-DPOLYBENCH_TIME=1"]
        self.assertEqual(_correctness_base_defines(defines), ["-DKEEP=1"])


if __name__ == "__main__":
    unittest.main()
