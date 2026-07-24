"""
Real IR-extraction tests for src/compiler_manager.py::CompilerRunner.extract_ir,
covering the C vs C++ language-selection fix (previously: C++ was recognized
only via a bare ".cpp" suffix check, and its IR-extraction command was
missing -Xclang -disable-O0-optnone entirely) and the c99 -> gnu99 fix
(previously: strict -std=c99 rejected real SPEC-shaped C sources that use
POSIX/BSD declarations like fileno(), which gnu99 exposes by default).

These load the real configs/config.yaml (the pinned LLVM 21 toolchain) via
ConfigLoader, exactly as optimize.py's main() does, rather than constructing
a CompilerRunner by hand -- a regression in config wiring (e.g. clang_cxx_path
pointing somewhere wrong again) would be caught here too, not just a
regression in the selection logic itself.

Run with:
    python3 -m unittest tests.test_compiler_runner_ir_extraction -v
from the repo root (comet/).
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from src.config import ConfigLoader
from src.compiler_manager import CompilerRunner


def _load_runner():
    loader = ConfigLoader(config_dir=str(Path(__file__).resolve().parent.parent / "configs"))
    config = loader.load_all()
    return CompilerRunner(config, None), config


def _verify_llvm21(path: str):
    try:
        r = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=30)
    except Exception as e:
        return f"{path} --version failed: {e}"
    if r.returncode != 0:
        return f"{path} --version exited {r.returncode}"
    if "21." not in r.stdout:
        return f"{path} is not LLVM 21.x: {r.stdout.splitlines()[0] if r.stdout else '(no output)'}"
    return None


_runner, _config = _load_runner()
_SKIP_REASON = (_verify_llvm21(_config.compiler.clang_path)
               or _verify_llvm21(_config.compiler.clang_cxx_path))


@unittest.skipIf(_SKIP_REASON, f"pinned LLVM 21 toolchain unavailable: {_SKIP_REASON}")
class TestExtractIrCLanguage(unittest.TestCase):
    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)

    def test_plain_c_still_extracts_ir(self):
        src = self.tmpdir / "plain.c"
        src.write_text("int add(int a, int b) { return a + b; }\n")
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, f"plain C extract_ir failed: {err}")
        self.assertTrue(Path(ir_path).exists())
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))

    def test_fileno_regression_gnu99_vs_c99(self):
        # The exact real bug reported: a SPEC-shaped C source using fileno()
        # (a POSIX declaration hidden by strict c99's feature-test macros)
        # failed under the pipeline's old -std=c99 in extract_ir's C branch.
        # gnu99 (matching src/build_utils.py::compile_c's existing choice
        # for the same reason) must expose it.
        src = self.tmpdir / "fileno_user.c"
        src.write_text(
            '#include <stdio.h>\n'
            'int get_fd(FILE *f) { return fileno(f); }\n'
        )
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, f"fileno() should compile under gnu99, got: {err}")
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))

    def test_fileno_would_fail_under_strict_c99(self):
        # Sanity check that this is a real, meaningful regression test and
        # not a tautology -- prove the OLD behavior (-std=c99) genuinely
        # rejects the same source extract_ir now accepts.
        src = self.tmpdir / "fileno_user2.c"
        src.write_text(
            '#include <stdio.h>\n'
            'int get_fd(FILE *f) { return fileno(f); }\n'
        )
        cmd = [_config.compiler.clang_path, "-std=c99", "-S", "-emit-llvm",
              "-Xclang", "-disable-O0-optnone", str(src), "-o", "/dev/null"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(r.returncode, 0,
                            "expected strict c99 to reject fileno() -- if this "
                            "now passes, the regression test above is meaningless")
        self.assertIn("fileno", r.stderr)

    def test_ir_has_optnone_stripped(self):
        # -disable-O0-optnone must actually take effect: without it, opt -O3
        # skips every pass on this function ("due to optnone attribute"),
        # which silently breaks all downstream pass-graph/IR-diff analysis.
        # (Deliberately NOT naming the source file anything containing
        # "optnone" -- the IR's ModuleID/source_filename lines embed the
        # input path verbatim, so a filename match would be a false
        # positive unrelated to the actual function attribute list.)
        src = self.tmpdir / "addfn.c"
        src.write_text("int add(int a, int b) { return a + b; }\n")
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, err)
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))
        attr_lines = [l for l in Path(ir_path).read_text().splitlines()
                     if l.startswith("attributes #")]
        self.assertTrue(attr_lines, "no 'attributes #N = {...}' lines found in IR at all")
        self.assertFalse(any("optnone" in l for l in attr_lines),
                         f"a function attribute list still carries optnone: {attr_lines}")


@unittest.skipIf(_SKIP_REASON, f"pinned LLVM 21 toolchain unavailable: {_SKIP_REASON}")
class TestExtractIrCxxLanguage(unittest.TestCase):
    """Covers the second half of the real bug: the C++ branch previously
    matched only a bare `.cpp` suffix (missing .cc/.cxx/.c++/.C) and was
    missing -Xclang -disable-O0-optnone entirely (present on the C branch,
    absent on the C++ one) -- meaning even a recognized .cpp file's IR would
    come back fully optnone'd and every pass-graph/IR-diff analysis on it
    would silently report "nothing optimizable" instead of a real answer."""

    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)

    def test_cpp_extension_extracts_ir(self):
        src = self.tmpdir / "k.cpp"
        src.write_text("int add(int a, int b) { return a + b; }\n")
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, f".cpp extract_ir failed: {err}")
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))

    def test_cc_extension_extracts_ir(self):
        # Previously unrecognized -- only ".cpp" was checked.
        src = self.tmpdir / "k.cc"
        src.write_text("int add(int a, int b) { return a + b; }\n")
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, f".cc extract_ir failed: {err}")
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))

    def test_cxx_extension_extracts_ir(self):
        src = self.tmpdir / "k.cxx"
        src.write_text("int add(int a, int b) { return a + b; }\n")
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, f".cxx extract_ir failed: {err}")
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))

    def test_cxx_ir_has_optnone_stripped(self):
        # This is the specific gap that existed before this fix: the C++
        # branch compiled successfully but WITHOUT -disable-O0-optnone.
        src = self.tmpdir / "sum_vector.cpp"
        src.write_text(
            "#include <vector>\n"
            "int sum(const std::vector<int>& v) { int s = 0; for (int x : v) s += x; return s; }\n"
        )
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, err)
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))
        attr_lines = [l for l in Path(ir_path).read_text().splitlines()
                      if l.startswith("attributes #")]
        self.assertTrue(attr_lines, "no function attribute lines found in C++ IR")
        self.assertFalse(any("optnone" in l for l in attr_lines),
                         f"C++ function attributes still carry optnone: {attr_lines}")

    def test_cxx_ir_is_genuinely_optimizable_by_opt_o3(self):
        # End-to-end proof, not just "no optnone string": feed the extracted
        # IR through the pinned opt-21 -O3 and confirm at least one real
        # pass actually runs on the function (this is what pass_graph.py/
        # ir_diff.py depend on for a C++ kernel to be analyzable at all).
        opt_path = _config.compiler.opt_path
        src = self.tmpdir / "loop.cpp"
        src.write_text(
            "int sum_squares(int *a, int n) {\n"
            "  int s = 0;\n"
            "  for (int i = 0; i < n; i++) s += a[i] * a[i];\n"
            "  return s;\n"
            "}\n"
        )
        ok, ir_path, err = _runner.extract_ir(str(src))
        self.assertTrue(ok, err)
        self.addCleanup(lambda: Path(ir_path).unlink(missing_ok=True))
        r = subprocess.run(
            [opt_path, "-passes=default<O3>", "-debug-pass-manager",
            "-disable-output", ir_path],
            capture_output=True, text=True, timeout=60)
        self.assertIn("Running pass:", r.stderr,
                     "opt -O3 ran no passes at all on the C++ kernel's IR -- "
                     "almost certainly still optnone'd")


if __name__ == "__main__":
    unittest.main()
