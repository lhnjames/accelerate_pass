"""Regression tests for ablation condition B (--no-compiler-feedback).

The scientific claim of the ablation is "the LLM saw NO compiler or hardware
feedback".  That claim is only as good as the enforcement, so these tests
assert on the actual rendered prompt text rather than on the flag plumbing:
if any future change re-introduces a leak (a new evidence field, a new prompt
section, a new auto-supplement path), the leak has to show up as a failure
here rather than as an unnoticed contamination of a published number.
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import optimize as O


def _rich_ev() -> dict:
    """An evidence dict populated in every feedback channel COMET collects."""
    return {
        "kernel_name": "kernel_demo",
        "kernel_text": "void kernel_demo(int n, double A[n][n]) {\n  /* body */\n}",
        "kline_start": 10,
        "kline_end": 20,
        "utils": None,
        "source_dir": ".",
        "correctness_mode": "numeric",
        # -- compiler feedback --
        "kernel_remarks": {"LoopVectorizePass": [
            {"type": "missed", "line": 12,
             "msg": "SECRETREMARK loop not vectorized"}]},
        "rich_remarks": {"missed": [{"pass": "LoopVectorizePass", "line": 12,
                                     "msg": "SECRETREMARK"}]},
        "missed_counts": {"LoopVectorizePass": 7},
        "kernel_passes": ["LoopVectorizePass", "LICMPass", "SECRETPASS"],
        "top_passes": [(1, "SECRETPASS", "x", 7, [])],
        "targeted_passes": [{"pass_name": "LICMPass", "missed_count": 3,
                             "params": ["-licm-max-num-uses-traversed"]}],
        "discovered_opts": {"LICMPass": [
            {"flag": "-licm-max-num-uses-traversed", "type": "uint",
             "desc": "SECRETOPT max uses"}]},
        "pass_graph": {"summary": "SECRETGRAPH 63 passes", "stats": {"unique_passes": 63}},
        "kernel_ir": "define void @kernel_demo() { ; SECRETIR\n ret void\n}",
        "ir_diff_info": ["SECRETDIFF vector_ops 4 -> 8"],
        "ir_pass_diffs": {"LICMPass": {"before": 1, "after": 2}},
        "pass_runtime_analysis": {"global_diagnosis": "SECRETAUDIT memory bound",
                                  "priority": ["LICMPass"],
                                  "allowed_flags": ["licm-max-num-uses-traversed"],
                                  "passes": []},
        "static_summary": "SECRETSTATIC stride-1 loops",
        # -- hardware feedback --
        "baseline_perf": {"ipc": 0.83, "llc_miss_pct": 22.0,
                          "bottleneck_hints": ["SECRETBOTTLENECK memory_bound"]},
        "baseline_stats": {"vector_ops": 12, "fmul": 30},
        "hotspot_target": "SECRETHOTSPOT",
        "hotspot_reason": "SECRETHOTSPOTREASON 91% of samples",
        "hotspot_targets": ["SECRETHOTSPOT"],
    }


# Every marker planted above.  None may survive into a no-feedback prompt.
_SECRETS = ["SECRETREMARK", "SECRETOPT", "SECRETGRAPH", "SECRETIR", "SECRETDIFF",
            "SECRETAUDIT", "SECRETSTATIC", "SECRETBOTTLENECK", "SECRETHOTSPOT",
            "SECRETHOTSPOTREASON", "SECRETPASS"]


class TestStripCompilerFeedback(unittest.TestCase):
    def test_every_feedback_channel_is_emptied(self):
        ev = O._strip_compiler_feedback(_rich_ev())
        for key in O._FEEDBACK_EV_KEYS:
            self.assertFalse(ev[key], f"{key} still carries feedback: {ev[key]!r}")
        for key in O._FEEDBACK_EV_KEYS_DELETE:
            self.assertNotIn(key, ev, f"{key} must be deleted, not blanked")

    def test_keys_are_emptied_not_deleted(self):
        # Downstream code indexes ev['kernel_passes'] etc. directly; deleting
        # instead of emptying would turn the ablation into a crash.
        ev = O._strip_compiler_feedback(_rich_ev())
        for key in O._FEEDBACK_EV_KEYS:
            self.assertIn(key, ev)

    def test_the_allowed_inputs_survive(self):
        # Condition B is "source + compiler version + O3 command + baseline
        # time + correctness contract + own history" -- the source and the
        # correctness contract live in ev and must NOT be stripped.
        ev = O._strip_compiler_feedback(_rich_ev())
        self.assertIn("kernel_demo", ev["kernel_text"])
        self.assertEqual(ev["correctness_mode"], "numeric")

    def test_it_reports_what_it_stripped(self):
        ev = O._strip_compiler_feedback(_rich_ev())
        self.assertEqual(ev["feedback_used"], "none")
        for key in ("kernel_remarks", "baseline_perf", "pass_graph",
                    "discovered_opts", "pass_runtime_analysis"):
            self.assertIn(key, ev["_feedback_stripped_keys"])


class TestNoFeedbackPromptHasNoLeak(unittest.TestCase):
    """The end-to-end claim: the text actually sent to the LLM is clean."""

    def _prompt(self, ev):
        history = O.OptimizationHistory()
        return O._build_agent_prompt(
            "kernel_demo", ev, cpu_info="CPU: test", cpu_cache="L1 32K",
            history=history, current_best_source=None, current_best_flags=[],
            step_num=1, max_steps=5)

    def test_full_condition_prompt_does_contain_the_feedback(self):
        # Guards the test itself: if the markers never reach the Full prompt,
        # their absence from the no-feedback prompt would prove nothing.
        prompt = self._prompt(_rich_ev())
        found = [s for s in _SECRETS if s in prompt]
        self.assertTrue(found, "Full-condition prompt carried no feedback markers "
                               "at all -- this test can no longer detect a leak")

    def test_no_feedback_prompt_contains_no_marker(self):
        prompt = self._prompt(O._strip_compiler_feedback(_rich_ev()))
        leaked = [s for s in _SECRETS if s in prompt]
        self.assertEqual(leaked, [],
                         f"compiler/hardware feedback leaked into the "
                         f"no-feedback prompt: {leaked}")

    def test_no_feedback_prompt_still_contains_the_source(self):
        prompt = self._prompt(O._strip_compiler_feedback(_rich_ev()))
        self.assertIn("kernel_demo", prompt)


class TestTryFlagsRuleIsConditionMatched(unittest.TestCase):
    """The two conditions must differ in EVIDENCE, not in what they're allowed
    to do.  Observed live in the first 3mm no-feedback smoke run: the Full
    condition's rule ("only pick flags the audit output listed; do not invent
    flags") was still being shown under condition B, where the audit list is
    empty by construction -- so the LLM correctly concluded it was forbidden
    from proposing anything and returned an empty flags list, burning a step.
    That measures our prompt, not the absence of feedback.
    """

    def _prompt(self, ev, no_feedback):
        history = O.OptimizationHistory()
        previous = O.NO_COMPILER_FEEDBACK
        O.NO_COMPILER_FEEDBACK = no_feedback
        try:
            return O._build_agent_prompt(
                "kernel_demo", ev, cpu_info="CPU: test", cpu_cache="L1 32K",
                history=history, current_best_source=None, current_best_flags=[],
                step_num=1, max_steps=5, forced_action="try_flags")
        finally:
            O.NO_COMPILER_FEEDBACK = previous

    def test_full_condition_still_restricts_flags_to_the_audit(self):
        prompt = self._prompt(_rich_ev(), no_feedback=False)
        self.assertIn("不要凭空发明 flag", prompt)

    def test_no_feedback_condition_does_not_forbid_proposing_flags(self):
        prompt = self._prompt(O._strip_compiler_feedback(_rich_ev()),
                              no_feedback=True)
        self.assertNotIn("不要凭空发明 flag", prompt,
                         "condition B must not inherit the Full condition's "
                         "audit-only restriction -- it has no audit, so the "
                         "rule reduces to 'propose nothing'")
        self.assertIn("不要因为缺少证据就交空的 flags 列表", prompt)


class TestAuditStageIsSkipped(unittest.TestCase):
    def test_module_flag_exists_and_defaults_off(self):
        # run_agent_step() gates run_pass_runtime_analysis() on this global;
        # defaulting it to True would silently disable the audit for the Full
        # condition, which is the more damaging direction of this bug.
        self.assertFalse(O.NO_COMPILER_FEEDBACK)

    def test_audit_call_is_guarded_by_the_flag(self):
        # Anchor on the audit call itself, then look at the nearest enclosing
        # `if forced_action == "try_flags"` above it -- there are several
        # unrelated try_flags branches in this file (prompt-guidance text,
        # empty-plan fallback) and matching the first one proves nothing.
        src = Path(O.__file__).read_text()
        call = src.index('ev["pass_runtime_analysis"] = run_pass_runtime_analysis(')
        guards = re.findall(r'if forced_action == "try_flags"[^\n]*:', src[:call])
        self.assertTrue(guards, "the try_flags audit guard moved or was renamed")
        self.assertIn("NO_COMPILER_FEEDBACK", guards[-1],
                      "the pass-audit stage is no longer gated on the ablation "
                      "flag -- condition B would leak audit-ranked flags")


if __name__ == "__main__":
    unittest.main()
