import unittest

import optimize


class PassAnalysisTests(unittest.TestCase):
    def _evidence(self):
        return {
            "kernel_passes": ["SLPVectorizerPass", "LICMPass"],
            "kernel_remarks": {
                "SLPVectorizerPass": [{"type": "missed", "line": 12,
                                        "msg": "cost too high"}],
            },
            "ir_pass_diffs": {
                "SLPVectorizerPass": {"changed": True, "skipped": False},
                "LICMPass": {"changed": False, "skipped": False},
            },
            "discovered_opts": {
                "SLPVectorizerPass": [{"flag": "--slp-threshold", "type": "int",
                                        "desc": "threshold"}],
                "LICMPass": [{"flag": "--licm-max-num-uses-traversed", "type": "uint",
                               "desc": "uses"}],
            },
            "targeted_passes": [{"pass_name": "SLPVectorizerPass"}],
            "baseline_stats": {"vector_ops": 2},
            "baseline_perf": {"bottleneck_hints": ["vectorization_gap"]},
            "correctness_mode": "hash",
            "utils": object(),
            "kernel_text": "void kernel(void) {}",
        }

    def test_normalizer_requires_discovered_numeric_flags(self):
        parsed = {
            "runtime_logic": "hot loop",
            "debug_combinations": [{"parameters": [
                {"flag": "--slp-threshold", "value": -4}
            ], "why": "lower cost"}],
            "passes": [
                {"pass": "SLPVectorizerPass", "purpose": "vectorize",
                 "debug_parameters": [
                     {"flag": "--slp-threshold", "candidates": [-4, 0]},
                     {"flag": "--invented-flag", "candidates": [1]},
                     {"flag": "--force-vector-width", "candidates": [8]},
                 ]},
                {"pass": "LICMPass", "purpose": "hoist",
                 "debug_parameters": []},
            ],
        }
        result = optimize._normalize_pass_runtime_analysis(parsed, self._evidence())
        self.assertEqual(2, len(result["passes"]))
        self.assertEqual(["-slp-threshold"],
                         [p["flag"] for p in result["debug_parameters"]])
        self.assertEqual([-4, 0], result["debug_parameters"][0]["candidates"])
        self.assertEqual("-slp-threshold",
                         result["debug_combinations"][0]["parameters"][0]["flag"])

    def test_evidence_contains_complete_pass_inventory_and_runtime(self):
        text = optimize._pass_runtime_evidence_text(
            "kernel", self._evidence(), baseline_time=12.5)
        self.assertIn("PASS=SLPVectorizerPass", text)
        self.assertIn("PASS=LICMPass", text)
        self.assertIn("baseline_time_ms", text)
        self.assertIn("LARGE_DATASET timing", text)

    def test_audit_call_returns_validated_parameters(self):
        old = optimize.run_skill_messages
        seen = {}
        try:
            def fake(llm, skill, messages, **kwargs):
                seen["skill"] = skill
                seen["prompt"] = messages[1]["content"]
                return ('{"runtime_logic":"x","global_diagnosis":"y",'
                        '"priority":["SLPVectorizerPass"],"passes":['
                        '{"pass":"SLPVectorizerPass","purpose":"v",'
                        '"debug_parameters":[{"flag":"--slp-threshold",'
                        '"candidates":[-4,0]}]},'
                        '{"pass":"LICMPass","purpose":"l",'
                        '"debug_parameters":[]}]}')
            optimize.run_skill_messages = fake
            result = optimize.run_pass_runtime_analysis(
                object(), "kernel", self._evidence(), max_tokens=100)
        finally:
            optimize.run_skill_messages = old
        self.assertEqual("pass-analysis", seen["skill"])
        self.assertIn("Complete O3 pass inventory", seen["prompt"])
        self.assertEqual(["-slp-threshold"],
                         [p["flag"] for p in result["debug_parameters"]])


if __name__ == "__main__":
    unittest.main()
