import ast
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_SKILLS = {
    "action-decision", "meta-planning", "failure-reflection",
    "rewrite-analysis", "source-rewrite", "precision-repair",
    "compile-repair", "parameter-tuning", "pass-analysis",
}


class SkillMigrationTests(unittest.TestCase):
    def _tree(self, filename):
        return ast.parse((ROOT / filename).read_text(encoding="utf-8"))

    def test_legacy_optimizer_modules_have_no_direct_llm_call(self):
        for filename in ("optimize.py", "tune_param.py", "tune_source.py"):
            for node in ast.walk(self._tree(filename)):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
                    continue
                if node.func.attr != "call":
                    continue
                if isinstance(node.func.value, ast.Name) and node.func.value.id in {
                    "llm", "_llm", "client"
                }:
                    self.fail(f"direct LLM call remains in {filename}:{node.lineno}")

    def test_all_required_skills_are_referenced_by_migration_calls(self):
        referenced = set()
        for filename in ("optimize.py", "tune_param.py", "tune_source.py"):
            for node in ast.walk(self._tree(filename)):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                if node.func.id != "run_skill_messages" or len(node.args) < 2:
                    continue
                skill = node.args[1]
                if isinstance(skill, ast.Constant) and isinstance(skill.value, str):
                    referenced.add(skill.value)
        self.assertEqual(REQUIRED_SKILLS, referenced)


if __name__ == "__main__":
    unittest.main()
