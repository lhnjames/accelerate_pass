"""
Regression tests for optimize.py's _canonical_debug_flag / _canonical_flag_key
-- the pass-audit whitelist matching bug flagged in the 2026-07-22 handoff:

    [Pass audit] 丢弃未被审计批准的参数: -mllvm -licm-max-num-uses-traversed

A real, audit-approved flag ("-licm-max-num-uses-traversed") was discarded
because the action-decision LLM sometimes returns it with "-mllvm" embedded
directly in the flag string as one token (rather than as its own separate
compiler argument), and only leading dashes were being stripped before the
audit-membership comparison -- "mllvm -licm-max-num-uses-traversed" never
matched the audit's own correctly-bare "licm-max-num-uses-traversed" key.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import optimize as O


class TestCanonicalDebugFlag(unittest.TestCase):
    def test_double_dash_becomes_single_dash(self):
        self.assertEqual(O._canonical_debug_flag("--licm-max-num-uses-traversed"),
                         "-licm-max-num-uses-traversed")

    def test_already_single_dash_is_unchanged(self):
        self.assertEqual(O._canonical_debug_flag("-licm-max-num-uses-traversed"),
                         "-licm-max-num-uses-traversed")

    def test_bare_name_gets_a_dash_added(self):
        self.assertEqual(O._canonical_debug_flag("licm-max-num-uses-traversed"),
                         "-licm-max-num-uses-traversed")

    def test_trailing_value_is_stripped(self):
        self.assertEqual(O._canonical_debug_flag("-licm-max-num-uses-traversed=32"),
                         "-licm-max-num-uses-traversed")

    def test_embedded_mllvm_prefix_is_stripped(self):
        # The exact bug: the LLM returned this as ONE string.
        self.assertEqual(
            O._canonical_debug_flag("-mllvm -licm-max-num-uses-traversed"),
            "-licm-max-num-uses-traversed")

    def test_embedded_double_dash_mllvm_prefix_is_stripped(self):
        self.assertEqual(
            O._canonical_debug_flag("--mllvm --licm-max-num-uses-traversed"),
            "-licm-max-num-uses-traversed")

    def test_embedded_mllvm_is_case_insensitive(self):
        self.assertEqual(
            O._canonical_debug_flag("-MLLVM -licm-max-num-uses-traversed"),
            "-licm-max-num-uses-traversed")


class TestCanonicalFlagKey(unittest.TestCase):
    def test_matches_regardless_of_embedded_mllvm(self):
        # This is the actual comparison key used for audit_allowed/
        # seen_spec_flags membership checks -- both spellings of the same
        # real flag must produce the identical key.
        plain = O._canonical_flag_key("-licm-max-num-uses-traversed")
        embedded = O._canonical_flag_key("-mllvm -licm-max-num-uses-traversed")
        self.assertEqual(plain, embedded)
        self.assertEqual(plain, "licm-max-num-uses-traversed")

    def test_reproduces_the_reported_handoff_bug_scenario(self):
        # audit_allowed as it would be built from a clean pass-audit result
        audit_allowed = {O._canonical_flag_key("-licm-max-num-uses-traversed")}
        # the action-decision LLM's malformed response for the same flag
        raw_flag_from_llm = "-mllvm -licm-max-num-uses-traversed"
        key = O._canonical_flag_key(raw_flag_from_llm)
        self.assertIn(key, audit_allowed,
                     "a real audit-approved flag must not be wrongly discarded "
                     "just because the LLM embedded -mllvm in the flag string")

    def test_double_dash_and_value_suffix_also_normalize_the_same(self):
        keys = {
            O._canonical_flag_key("--licm-max-num-uses-traversed"),
            O._canonical_flag_key("-licm-max-num-uses-traversed"),
            O._canonical_flag_key("-licm-max-num-uses-traversed=64"),
            O._canonical_flag_key("-mllvm -licm-max-num-uses-traversed=64"),
        }
        self.assertEqual(keys, {"licm-max-num-uses-traversed"})


if __name__ == "__main__":
    unittest.main()
