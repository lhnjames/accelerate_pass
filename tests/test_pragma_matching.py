"""Tests for _apply_pragma_hints -- attaching #pragma clang loop to the right loop.

"未找到匹配的 for 循环前缀" was the single biggest source of wasted agent steps
in the corpus: 108 occurrences, more than compile failures (89) and correctness
rejections (15) combined. Most were AMBIGUITY, not absence -- a PolyBench file
contains init_array, print_array, main and kernel_xxx whose loop headers are
frequently identical, so a perfectly legal hint matched several lines and was
rejected outright rather than risk annotating the wrong loop.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from optimize import _apply_pragma_hints

SRC = """
static void init_array(int ni, int nj, double A[1][1]) {
  for (i = 0; i < ni; i++)
    for (j = 0; j < nj; j++)
      A[i][j] = 0;
}
static void kernel_2mm(int ni, int nj, int nk, double A[1][1]) {
  for (i = 0; i < ni; i++)
    for (j = 0; j < nj; j++)
      for (k = 0; k < nk; k++)
        A[i][j] += B[i][k] * C[k][j];
}
static void print_array(int ni) {
  for (i = 0; i < ni; i++) printf("%f", A[i]);
}
"""
PRAGMA = "#pragma clang loop vectorize(enable)"


def _annotated_loop(out):
    lines = out.split("\n")
    for i, l in enumerate(lines):
        if l.strip().startswith("#pragma"):
            return lines[i + 1].strip()
    return None


class TestKernelScoping(unittest.TestCase):
    def test_ambiguous_across_file_is_unique_inside_kernel(self):
        h = [{"loop_prefix": "for (i = 0; i < ni; i++)", "pragma": PRAGMA}]
        # The same header appears in init_array, kernel_2mm and print_array.
        self.assertEqual(_apply_pragma_hints(SRC, h), SRC)          # ambiguous -> refused
        out = _apply_pragma_hints(SRC, h, "kernel_2mm")
        self.assertNotEqual(out, SRC)
        self.assertEqual(_annotated_loop(out), "for (i = 0; i < ni; i++)")

    def test_annotation_lands_inside_the_kernel_body(self):
        h = [{"loop_prefix": "for (i = 0; i < ni; i++)", "pragma": PRAGMA}]
        out = _apply_pragma_hints(SRC, h, "kernel_2mm")
        idx = [i for i, l in enumerate(out.split("\n")) if l.strip().startswith("#pragma")][0]
        body = out.split("\n")
        kstart = [i for i, l in enumerate(body) if "kernel_2mm" in l][0]
        kend = [i for i, l in enumerate(body) if "print_array" in l][0]
        self.assertTrue(kstart < idx < kend, "pragma must be inside kernel_2mm")


class TestInductionVariableTier(unittest.TestCase):
    def test_wrong_bound_expression_still_matches(self):
        # The model writes the PolyBench macro name instead of the parameter.
        h = [{"loop_prefix": "for (k = 0; k < _PB_NK; k++)",
              "pragma": "#pragma clang loop unroll_count(4)"}]
        out = _apply_pragma_hints(SRC, h, "kernel_2mm")
        self.assertEqual(_annotated_loop(out), "for (k = 0; k < nk; k++)")

    def test_declared_induction_variable_matches(self):
        src = SRC.replace("for (k = 0; k < nk; k++)", "for (int k = 0; k < nk; k++)")
        h = [{"loop_prefix": "for (k = 0; k < nk; k++)", "pragma": PRAGMA}]
        out = _apply_pragma_hints(src, h, "kernel_2mm")
        self.assertEqual(_annotated_loop(out), "for (int k = 0; k < nk; k++)")


class TestSafetyPreserved(unittest.TestCase):
    def test_nonexistent_loop_is_still_refused(self):
        h = [{"loop_prefix": "for (zz = 0; zz < n; zz++)", "pragma": PRAGMA}]
        self.assertEqual(_apply_pragma_hints(SRC, h, "kernel_2mm"), SRC)

    def test_ambiguity_inside_the_kernel_is_still_refused(self):
        # Two identical headers within one kernel body: guessing is not allowed.
        src = SRC.replace("for (j = 0; j < nj; j++)\n      for (k = 0; k < nk; k++)",
                          "for (j = 0; j < nj; j++)\n      for (j = 0; j < nj; j++)")
        h = [{"loop_prefix": "for (j = 0; j < nj; j++)", "pragma": PRAGMA}]
        self.assertEqual(_apply_pragma_hints(src, h, "kernel_2mm"), src)

    def test_unknown_kernel_name_falls_back_to_whole_file(self):
        h = [{"loop_prefix": "for (k = 0; k < nk; k++)", "pragma": PRAGMA}]
        out = _apply_pragma_hints(SRC, h, "kernel_does_not_exist")
        self.assertEqual(_annotated_loop(out), "for (k = 0; k < nk; k++)")


if __name__ == "__main__":
    unittest.main()
