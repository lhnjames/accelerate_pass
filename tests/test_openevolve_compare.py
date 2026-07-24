"""Tests for the OpenEvolve comparison harness's pure logic.

The timed evaluation itself needs the ARM host + the LLM, but two things must be
correct regardless of environment and are cheap to pin down here:
  1. EVOLVE-BLOCK markers wrap the kernel function and nothing else -- if they
     land in the wrong place, OpenEvolve either edits scaffolding it shouldn't
     or cannot edit the kernel at all, silently invalidating the comparison.
  2. The generated config carries the SAME model/iteration budget as COMET --
     the whole point of the comparison is that only the search strategy differs.
"""
import importlib.util
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

_spec = importlib.util.spec_from_file_location(
    "gen_inputs", PROJECT_ROOT / "scripts" / "openevolve_compare" / "gen_inputs.py")
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)


_SAMPLE = """\
#include <stdio.h>
static void init_array(int n, double A[n]) { for (int i=0;i<n;i++) A[i]=i; }

static
void kernel_demo(int n, double A[n], double B[n]) {
  int i;
  for (i = 0; i < n; i++)
    A[i] = A[i] + B[i];
}

int main() { return 0; }
"""


class TestMarkerInsertion(unittest.TestCase):
    def test_markers_wrap_the_kernel_only(self):
        out = gen._insert_markers(_SAMPLE, "kernel_demo")
        self.assertIn(gen.MARK_START, out)
        self.assertIn(gen.MARK_END, out)
        start = out.index(gen.MARK_START)
        end = out.index(gen.MARK_END)
        between = out[start:end]
        # the kernel is inside the block ...
        self.assertIn("kernel_demo", between)
        self.assertIn("A[i] = A[i] + B[i];", between)
        # ... and the scaffolding is NOT
        self.assertNotIn("init_array", between)
        self.assertNotIn("int main", between)

    def test_markers_do_not_duplicate_or_drop_code(self):
        out = gen._insert_markers(_SAMPLE, "kernel_demo")
        stripped = out.replace(gen.MARK_START + "\n", "").replace("\n" + gen.MARK_END, "")
        self.assertEqual(stripped, _SAMPLE)

    def test_missing_kernel_raises(self):
        with self.assertRaises(ValueError):
            gen._insert_markers(_SAMPLE, "kernel_nonexistent")

    def test_double_marking_refused(self):
        once = gen._insert_markers(_SAMPLE, "kernel_demo")
        with self.assertRaises(ValueError):
            gen._insert_markers(once, "kernel_demo")


class TestConfigMatchesComet(unittest.TestCase):
    def test_config_uses_comet_model_and_iteration_budget(self):
        cfg = gen.CONFIG_YAML.format(
            stem="demo", rounds=3, base_url="https://api.deepseek.com",
            model="deepseek-v4-pro", temperature=0.6, max_tokens=4000,
            eval_timeout=600)
        self.assertIn("max_iterations: 3", cfg)         # == COMET --rounds 3
        self.assertIn("deepseek-v4-pro", cfg)           # same model as COMET
        self.assertIn("api.deepseek.com", cfg)
        self.assertIn("${DEEPSEEK_API_KEY}", cfg)       # key via env, never inlined
        self.assertIn("parallel_evaluations: 1", cfg)   # serial timing, no contention
        self.assertIn("language: c", cfg)


if __name__ == "__main__":
    unittest.main()
