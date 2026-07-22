import unittest

from tune_param import is_cost_model_override, is_unsafe_discovered_option


class TestCostModelOverrideFilter(unittest.TestCase):
    def test_interior_force_token_is_rejected(self):
        self.assertTrue(is_cost_model_override("--unroll-force-peel-count"))

    def test_force_prefix_and_disable_tokens_are_rejected(self):
        self.assertTrue(is_cost_model_override("force-vector-width"))
        self.assertTrue(is_cost_model_override("--disable-loop-unrolling"))
        self.assertTrue(is_cost_model_override("--licm-disable-promotion"))

    def test_explicit_testing_only_unroll_counts_are_rejected(self):
        self.assertTrue(is_cost_model_override("--unroll-peel-count"))
        self.assertTrue(is_cost_model_override("--unroll-max-count"))
        self.assertTrue(is_cost_model_override("--unroll-full-max-count"))

    def test_testing_purpose_description_is_rejected_despite_llvm_typo(self):
        self.assertTrue(is_unsafe_discovered_option(
            "--some-new-option", "Set a limit, for testing purposes"))
        self.assertTrue(is_unsafe_discovered_option(
            "--some-new-option", "Set a limit, fortesting purposes"))

    def test_regular_cost_thresholds_remain_available(self):
        self.assertFalse(is_cost_model_override("--unroll-threshold"))
        self.assertFalse(is_cost_model_override("--slp-max-vf"))
        self.assertFalse(is_cost_model_override("--licm-mssa-max-acc-promotion"))
        self.assertFalse(is_unsafe_discovered_option(
            "--unroll-threshold", "The cost threshold for loop unrolling"))

    def test_embedded_mllvm_prefix_does_not_hide_a_real_override(self):
        # Observed live: an action-decision response can embed "-mllvm"
        # directly inside the flag string as ONE token instead of a
        # separate compiler argument (e.g. "-mllvm -unroll-force-peel-count").
        # Before stripping this prefix, name.split("-") smashed "mllvm" and
        # the real option into one unrecognizable blob, so a genuine
        # force/disable override could slip past the blacklist check.
        self.assertTrue(is_cost_model_override("-mllvm -unroll-force-peel-count"))
        self.assertTrue(is_cost_model_override("--mllvm --disable-loop-unrolling"))

    def test_embedded_mllvm_prefix_does_not_falsely_reject_a_safe_flag(self):
        self.assertFalse(is_cost_model_override("-mllvm -licm-max-num-uses-traversed"))
        self.assertFalse(is_cost_model_override("-mllvm -unroll-threshold"))


if __name__ == "__main__":
    unittest.main()
