import tempfile
import unittest
from pathlib import Path

from src.skill_executor import (
    SKILLS_ROOT, SkillNotFoundError, SkillRecorder, SkillRuntime,
    build_skill_messages, load_skill, load_skill_definition, run_skill,
    set_skill_recorder,
)


class RecordingLLM:
    def __init__(self):
        self.args = None

    def call(self, messages, temperature=None, max_tokens=None, timeout=None):
        self.args = (messages, temperature, max_tokens, timeout)
        return "ok"


class TestSkillExecutor(unittest.TestCase):
    def setUp(self):
        set_skill_recorder(None)

    def test_all_required_skills_exist(self):
        required = {
            "action-decision", "meta-planning", "failure-reflection",
            "rewrite-analysis", "source-rewrite", "precision-repair",
            "compile-repair", "parameter-tuning", "pass-analysis",
        }
        self.assertEqual(required, {p.parent.name for p in SKILLS_ROOT.glob("*/SKILL.md")})
        for name in required:
            self.assertTrue(load_skill(name).startswith("# "))

    def test_build_messages_names_active_skill_and_keeps_task(self):
        messages = build_skill_messages("meta-planning", "measured context")
        self.assertEqual([m["role"] for m in messages], ["system", "user"])
        self.assertIn("ACTIVE SKILL: meta-planning", messages[0]["content"])
        self.assertEqual(messages[1]["content"], "measured context")

    def test_run_skill_forwards_controls(self):
        llm = RecordingLLM()
        result = run_skill(llm, "failure-reflection", "failure", temperature=0.2,
                           max_tokens=900, timeout=30)
        self.assertEqual(result, "ok")
        self.assertEqual(llm.args[1:], (0.2, 900, 30))

    def test_skill_hash_and_invocation_order_are_recorded(self):
        llm = RecordingLLM()
        recorder = SkillRecorder()
        run_skill(llm, "meta-planning", "first", recorder=recorder)
        run_skill(llm, "failure-reflection", "second", recorder=recorder)
        metadata = recorder.to_dict()
        self.assertEqual(
            [item["name"] for item in metadata["invocations"]],
            ["meta-planning", "failure-reflection"])
        self.assertEqual(metadata["counts"]["meta-planning"], 1)
        self.assertEqual(
            metadata["skill_sha256"]["meta-planning"],
            load_skill_definition("meta-planning").sha256)

    def test_skills_off_ablation_omits_skill_policy_and_hash(self):
        llm = RecordingLLM()
        runtime = SkillRuntime(llm, enabled=False)
        self.assertEqual(runtime.run("source-rewrite", "task"), "ok")
        system = llm.args[0][0]["content"]
        self.assertNotIn("ACTIVE SKILL", system)
        self.assertNotIn(load_skill("source-rewrite"), system)
        metadata = runtime.metadata()
        self.assertFalse(metadata["skills_enabled"])
        self.assertIsNone(metadata["invocations"][0]["sha256"])

    def test_process_recorder_can_switch_skills_off_for_legacy_call_sites(self):
        llm = RecordingLLM()
        recorder = SkillRecorder()
        set_skill_recorder(recorder, enabled=False)
        try:
            run_skill(llm, "action-decision", "task")
            self.assertNotIn("ACTIVE SKILL", llm.args[0][0]["content"])
            self.assertFalse(recorder.to_dict()["invocations"][0]["skills_enabled"])
        finally:
            set_skill_recorder(None)

    def test_missing_skill_fails_closed(self):
        with self.assertRaises(SkillNotFoundError):
            load_skill("does-not-exist")

    def test_path_traversal_is_rejected(self):
        with self.assertRaises(ValueError):
            load_skill("../outside")

    def test_custom_skill_root(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "custom" / "SKILL.md"
            path.parent.mkdir()
            path.write_text("# Custom\n\nDo the task.", encoding="utf-8")
            self.assertIn("Do the task", load_skill("custom", td))


if __name__ == "__main__":
    unittest.main()
