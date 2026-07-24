"""
Tests for the C vs C++ compiler-selection logic added to src/build_utils.py
(derive_cxx_compiler_path / is_cxx_source / select_compiler / compile_c's
clang_cxx_path support). Covers both pure unit tests (no filesystem/compiler
needed) and integration tests that actually invoke the local clang/clang++
binaries, so a green run here means C++ sources genuinely compile end to
end, not just that the branch logic looks right.

Run with:
    python3 -m unittest tests.test_cxx_compiler_selection -v
from the repo root (comet/).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from src.build_utils import (
    CompilerNotFoundError,
    compile_c,
    derive_cxx_compiler_path,
    get_default_cxx_compiler,
    is_cxx_source,
    select_compiler,
    set_default_cxx_compiler,
)


# ── Pure unit tests: derive_cxx_compiler_path ────────────────────────────────

class TestDeriveCxxCompilerPath(unittest.TestCase):
    def test_versioned(self):
        self.assertEqual(derive_cxx_compiler_path("/usr/bin/clang-11"),
                         "/usr/bin/clang++-11")

    def test_versioned_other_number(self):
        self.assertEqual(derive_cxx_compiler_path("/usr/bin/clang-21"),
                         "/usr/bin/clang++-21")

    def test_unversioned(self):
        self.assertEqual(derive_cxx_compiler_path("/usr/bin/clang"),
                         "/usr/bin/clang++")

    def test_preserves_directory(self):
        self.assertEqual(
            derive_cxx_compiler_path("/opt/custom/dir/clang-14"),
            "/opt/custom/dir/clang++-14")

    def test_unrecognized_name_returns_none(self):
        # A custom-named binary (not matching clang / clang-N) can't be
        # safely guessed at -- must fall back to an explicit override.
        self.assertIsNone(derive_cxx_compiler_path("/usr/bin/my-custom-cc"))

    def test_never_reproduces_the_historical_bug(self):
        # The pre-existing config had "clang-11++" (wrong -- doesn't exist
        # on any real install; the correct name is "clang++-11"). Guard
        # against ever regressing back to that shape.
        result = derive_cxx_compiler_path("/usr/bin/clang-11")
        self.assertNotEqual(result, "/usr/bin/clang-11++")
        self.assertEqual(result, "/usr/bin/clang++-11")


# ── Pure unit tests: is_cxx_source ────────────────────────────────────────────

class TestIsCxxSource(unittest.TestCase):
    def test_c_extensions_are_not_cxx(self):
        for ext in (".c", ".h"):
            with self.subTest(ext=ext):
                self.assertFalse(is_cxx_source(f"foo{ext}"))

    def test_cxx_extensions(self):
        for ext in (".cc", ".cpp", ".cxx", ".c++", ".C", ".CPP"):
            with self.subTest(ext=ext):
                self.assertTrue(is_cxx_source(f"foo{ext}"))

    def test_accepts_path_objects(self):
        self.assertTrue(is_cxx_source(Path("bar.cpp")))
        self.assertFalse(is_cxx_source(Path("bar.c")))


# ── Pure-logic tests: select_compiler (filesystem-independent branches) ──────

class TestSelectCompilerAllC(unittest.TestCase):
    def test_all_c_sources_return_clang_path_unchanged(self):
        # This is the entire pipeline's existing behavior for every kernel
        # compiled so far (PolyBench/TSVC/cBench/SPEC-C) -- must stay
        # byte-for-byte identical regardless of whether clang++ exists at
        # all, since these callers never had a C++ concept before.
        compiler, is_cxx = select_compiler(
            ["kernel.c", "polybench.c"], "/usr/bin/clang-11",
            clang_cxx_path="/does/not/exist")
        self.assertEqual(compiler, "/usr/bin/clang-11")
        self.assertFalse(is_cxx)

    def test_all_c_sources_ignore_missing_cxx_entirely(self):
        # Even with clang_cxx_path=None and an unresolvable clang_path name,
        # pure-C sources must never even attempt C++ resolution.
        compiler, is_cxx = select_compiler(
            ["a.c"], "/usr/bin/my-custom-cc", clang_cxx_path=None)
        self.assertEqual(compiler, "/usr/bin/my-custom-cc")
        self.assertFalse(is_cxx)


class TestSelectCompilerCxxWithRealBinaries(unittest.TestCase):
    """Uses a temp directory with fake executable files standing in for
    clang/clang++ binaries, so resolution-order behavior (explicit override
    beats derived, derived is used when no override, error when neither
    exists) can be tested without depending on what's actually installed."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)
        # Defensive against test-order leakage: these tests assert behavior
        # for "no registered default", which would silently break if some
        # other test left a global default registered (set_default_cxx_
        # compiler is process-wide mutable state -- see its own docstring).
        self.addCleanup(set_default_cxx_compiler, get_default_cxx_compiler())
        set_default_cxx_compiler(None)

    def _touch_executable(self, name: str) -> str:
        p = Path(self.tmpdir) / name
        p.write_text("#!/bin/sh\ntrue\n")
        p.chmod(0o755)
        return str(p)

    def test_mixed_c_and_cxx_sources_trigger_cxx_path(self):
        clang = self._touch_executable("clang-11")
        clangxx = self._touch_executable("clang++-11")
        compiler, is_cxx = select_compiler(["a.c", "b.cpp"], clang)
        self.assertEqual(compiler, clangxx)
        self.assertTrue(is_cxx)

    def test_explicit_override_wins_over_derived(self):
        clang = self._touch_executable("clang-11")
        self._touch_executable("clang++-11")           # derivable, but...
        custom_cxx = self._touch_executable("my-clang++")  # ...override wins
        compiler, is_cxx = select_compiler(
            ["a.cpp"], clang, clang_cxx_path=custom_cxx)
        self.assertEqual(compiler, custom_cxx)
        self.assertTrue(is_cxx)

    def test_falls_back_to_derived_when_override_missing(self):
        clang = self._touch_executable("clang-11")
        clangxx = self._touch_executable("clang++-11")
        compiler, is_cxx = select_compiler(
            ["a.cpp"], clang, clang_cxx_path="/nonexistent/path")
        self.assertEqual(compiler, clangxx)
        self.assertTrue(is_cxx)

    def test_raises_when_nothing_resolvable(self):
        clang = self._touch_executable("clang-11")
        # No clang++-11 created in tmpdir, no override given.
        with self.assertRaises(CompilerNotFoundError) as ctx:
            select_compiler(["a.cpp"], clang, clang_cxx_path=None)
        self.assertIn("clang++", str(ctx.exception))

    def test_raises_when_override_and_derived_both_missing(self):
        clang = self._touch_executable("clang-11")
        with self.assertRaises(CompilerNotFoundError):
            select_compiler(["a.cpp"], clang, clang_cxx_path="/also/missing")


class TestDefaultCxxCompilerRegistration(unittest.TestCase):
    """set_default_cxx_compiler() is how optimize.py/tune_param.py/
    tune_source.py's main() functions register configs/config.yaml's
    EXPLICIT compiler.clang_cxx_path as the process-wide default, so the
    ~15+ call sites that only have `clang` (not a clang_cxx parameter) in
    scope still resolve C++ sources against the real configured compiler
    instead of silently falling back to a naming-convention guess. Verifies
    the full resolution order: explicit arg > registered default > derived."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)
        self.addCleanup(set_default_cxx_compiler, get_default_cxx_compiler())
        set_default_cxx_compiler(None)

    def _touch_executable(self, name: str) -> str:
        p = Path(self.tmpdir) / name
        p.write_text("#!/bin/sh\ntrue\n")
        p.chmod(0o755)
        return str(p)

    def test_registered_default_is_used_when_no_explicit_arg(self):
        clang = self._touch_executable("clang-11")
        self._touch_executable("clang++-11")            # derivable, but...
        registered = self._touch_executable("registered-clang++")
        set_default_cxx_compiler(registered)
        compiler, is_cxx = select_compiler(["a.cpp"], clang)  # no explicit arg
        self.assertEqual(compiler, registered)
        self.assertTrue(is_cxx)

    def test_explicit_arg_still_wins_over_registered_default(self):
        clang = self._touch_executable("clang-11")
        registered = self._touch_executable("registered-clang++")
        explicit = self._touch_executable("explicit-clang++")
        set_default_cxx_compiler(registered)
        compiler, is_cxx = select_compiler(["a.cpp"], clang, clang_cxx_path=explicit)
        self.assertEqual(compiler, explicit)

    def test_registered_default_beats_derivation(self):
        clang = self._touch_executable("clang-11")
        self._touch_executable("clang++-11")  # would be derived if reached
        registered = self._touch_executable("registered-clang++")
        set_default_cxx_compiler(registered)
        compiler, _ = select_compiler(["a.cpp"], clang)
        self.assertEqual(compiler, registered,
                         "registered default must be checked BEFORE derivation")

    def test_no_default_registered_falls_through_to_derivation(self):
        clang = self._touch_executable("clang-11")
        clangxx = self._touch_executable("clang++-11")
        self.assertIsNone(get_default_cxx_compiler())
        compiler, _ = select_compiler(["a.cpp"], clang)
        self.assertEqual(compiler, clangxx)

    def test_stale_registered_default_that_no_longer_exists_is_skipped(self):
        # If a registered default path stops existing (e.g. a differently-
        # configured run), resolution must fall through to derivation rather
        # than erroring on a dangling path.
        clang = self._touch_executable("clang-11")
        clangxx = self._touch_executable("clang++-11")
        set_default_cxx_compiler("/no/longer/there")
        compiler, _ = select_compiler(["a.cpp"], clang)
        self.assertEqual(compiler, clangxx)


# ── Integration tests: real compilation (skipped if compilers absent) ────────

_ENFORCED_LLVM_VERSION = "21"

# Hard constraint: this project's toolchain is pinned to LLVM 21 only
# (configs/config.yaml -> compiler.clang_path/clang_cxx_path point at the
# project-vendored scripts/toolchain/clang-21 / clang++-21 wrappers, which
# in turn exec /home/hanning/Software/llvm-21/usr/bin/clang(++)-21). These
# integration tests must exercise that EXACT toolchain, not "whichever
# clang happens to be on PATH" (shutil.which("clang") would silently find
# the system's clang-11 on this machine and defeat the whole point of the
# constraint). If the pinned LLVM 21 wrappers are missing, not executable,
# or somehow report a different version, these tests SKIP with an explicit
# reason -- they must never silently substitute a different LLVM version.
_REPO_ROOT = Path(__file__).resolve().parent.parent
_CLANG = str(_REPO_ROOT / "scripts" / "toolchain" / "clang-21")
_CLANGXX = str(_REPO_ROOT / "scripts" / "toolchain" / "clang++-21")


def _verify_llvm21_toolchain(clang_path: str) -> Optional[str]:
    """Returns None if `clang_path` is a working, genuinely-LLVM-21 compiler;
    otherwise returns a human-readable reason it isn't, for use as a skip
    message. Never falls back to trying a different binary."""
    p = Path(clang_path)
    if not p.exists():
        return f"{clang_path} does not exist"
    if not os.access(p, os.X_OK):
        return f"{clang_path} exists but is not executable"
    try:
        result = subprocess.run([str(p), "--version"], capture_output=True,
                                text=True, timeout=30)
    except Exception as e:
        return f"{clang_path} --version failed to run: {e}"
    if result.returncode != 0:
        return f"{clang_path} --version exited {result.returncode}: {result.stderr[:200]}"
    if f" {_ENFORCED_LLVM_VERSION}." not in result.stdout and \
       f"-{_ENFORCED_LLVM_VERSION}" not in Path(clang_path).name:
        return (f"{clang_path} reports a version other than "
                f"{_ENFORCED_LLVM_VERSION}: {result.stdout.splitlines()[0] if result.stdout else '(no output)'}")
    return None


_CLANG_SKIP_REASON = _verify_llvm21_toolchain(_CLANG)
_CLANGXX_SKIP_REASON = _verify_llvm21_toolchain(_CLANGXX)
_SKIP_REASON = _CLANG_SKIP_REASON or _CLANGXX_SKIP_REASON


@unittest.skipIf(_SKIP_REASON,
                 f"pinned LLVM 21 toolchain unavailable, NOT falling back to another "
                 f"version: {_SKIP_REASON}")
class TestCompileCEndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)

    def test_c_source_compiles_and_runs_unaffected(self):
        src = self.tmpdir / "hello.c"
        src.write_text('#include <stdio.h>\n'
                       'int main(void) { printf("42\\n"); return 0; }\n')
        out_bin = self.tmpdir / "hello_c"
        ok, err = compile_c(_CLANG, [str(src)], [], [], str(out_bin))
        self.assertTrue(ok, f"C compile failed: {err}")
        result = subprocess.run([str(out_bin)], capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "42")

    def test_cxx_source_compiles_and_runs_via_derived_compiler(self):
        # No clang_cxx_path passed -- must derive clang++-N from clang-N
        # entirely on its own and still produce a working binary.
        src = self.tmpdir / "hello.cpp"
        src.write_text('#include <iostream>\n'
                       'int main() { std::cout << 42 << std::endl; return 0; }\n')
        out_bin = self.tmpdir / "hello_cxx"
        ok, err = compile_c(_CLANG, [str(src)], [], [], str(out_bin))
        self.assertTrue(ok, f"C++ compile via derived clang++ failed: {err}")
        result = subprocess.run([str(out_bin)], capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "42")

    def test_cxx_source_compiles_via_explicit_override(self):
        src = self.tmpdir / "hello2.cpp"
        src.write_text('#include <iostream>\n'
                       'int main() { std::cout << "ok" << std::endl; return 0; }\n')
        out_bin = self.tmpdir / "hello_cxx2"
        ok, err = compile_c(_CLANG, [str(src)], [], [], str(out_bin),
                            clang_cxx_path=_CLANGXX)
        self.assertTrue(ok, f"C++ compile via explicit override failed: {err}")
        result = subprocess.run([str(out_bin)], capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "ok")

    def test_gnu99_never_reaches_the_cxx_frontend(self):
        # A C++ file using a feature -std=gnu99 would choke on if it somehow
        # got passed through (this would fail differently / at a different
        # stage if the C-only flag leaked into the clang++ invocation --
        # clang++ itself rejects -std=gnu99 outright with a hard "not
        # allowed with 'C++'" error regardless of the source, so a *successful*
        # compile here is direct proof the flag was correctly omitted).
        src = self.tmpdir / "hello3.cpp"
        src.write_text('int main() { return 0; }\n')
        out_bin = self.tmpdir / "hello_cxx3"
        ok, err = compile_c(_CLANG, [str(src)], [], [], str(out_bin))
        self.assertTrue(ok, f"unexpected failure (possible -std=gnu99 leak): {err}")
        self.assertNotIn("gnu99", err)

    def test_mixed_c_and_cxx_sources_link_together(self):
        # Real C++ SPEC benchmarks mix .c helper files with .cpp files in
        # one build -- the whole thing must compile+link as C++ (for the
        # C++ runtime) while still correctly parsing the .c file as C.
        c_src = self.tmpdir / "helper.c"
        c_src.write_text('int add_one(int x) { return x + 1; }\n')
        cxx_src = self.tmpdir / "main.cpp"
        cxx_src.write_text(
            '#include <iostream>\n'
            'extern "C" int add_one(int);\n'
            'int main() { std::cout << add_one(41) << std::endl; return 0; }\n')
        out_bin = self.tmpdir / "mixed"
        ok, err = compile_c(_CLANG, [str(c_src), str(cxx_src)], [], [], str(out_bin))
        self.assertTrue(ok, f"mixed C/C++ compile failed: {err}")
        result = subprocess.run([str(out_bin)], capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "42")

    def test_binaries_are_genuinely_llvm21(self):
        # Belt-and-suspenders on top of the module-level skip guard: prove
        # the actual binary invoked to produce a passing result above really
        # is LLVM 21, not some other version that happened to satisfy the
        # weaker name-based check in _verify_llvm21_toolchain.
        for path in (_CLANG, _CLANGXX):
            result = subprocess.run([path, "--version"], capture_output=True, text=True)
            self.assertIn("21.", result.stdout,
                          f"{path} --version did not report an LLVM 21.x build: {result.stdout!r}")


@unittest.skipIf(_SKIP_REASON, f"pinned LLVM 21 toolchain unavailable: {_SKIP_REASON}")
class TestConfigYamlToolchainResolution(unittest.TestCase):
    """Proves the EXACT paths configs/config.yaml declares (not a stand-in)
    resolve correctly through select_compiler()/derive_cxx_compiler_path(),
    end to end, for both the C and the C++ side of the pinned LLVM 21
    toolchain -- this is what optimize.py/tune_param.py/tune_source.py
    actually load at runtime via ConfigLoader."""

    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmpdir, ignore_errors=True)

    def _load_configured_compiler_paths(self):
        sys.path.insert(0, str(_REPO_ROOT))
        from src.config import ConfigLoader
        loader = ConfigLoader(config_dir=str(_REPO_ROOT / "configs"))
        cfg = loader.load_all()
        return cfg.compiler.clang_path, cfg.compiler.clang_cxx_path

    def test_configured_paths_match_pinned_wrappers(self):
        clang_path, clang_cxx_path = self._load_configured_compiler_paths()
        self.assertEqual(clang_path, _CLANG,
                         "configs/config.yaml's clang_path no longer points at the "
                         "pinned LLVM 21 wrapper -- constraint may have regressed")
        self.assertEqual(clang_cxx_path, _CLANGXX,
                         "configs/config.yaml's clang_cxx_path no longer points at the "
                         "pinned LLVM 21 wrapper -- constraint may have regressed")

    def test_derive_matches_configured_cxx_path_even_without_it(self):
        # If clang_cxx_path were ever removed from config.yaml, the naming-
        # convention auto-derivation must still land on the exact same
        # pinned LLVM 21 clang++-21 wrapper, not silently walk down to
        # something else.
        clang_path, clang_cxx_path = self._load_configured_compiler_paths()
        self.assertEqual(derive_cxx_compiler_path(clang_path), clang_cxx_path)

    def test_c_and_cxx_both_compile_via_configured_paths(self):
        clang_path, clang_cxx_path = self._load_configured_compiler_paths()

        c_src = self.tmpdir / "k.c"
        c_src.write_text('int main(){return 0;}\n')
        c_bin = self.tmpdir / "k_c"
        ok, err = compile_c(clang_path, [str(c_src)], [], [], str(c_bin),
                            clang_cxx_path=clang_cxx_path)
        self.assertTrue(ok, f"C compile via configs/config.yaml paths failed: {err}")

        cxx_src = self.tmpdir / "k.cpp"
        cxx_src.write_text('int main(){return 0;}\n')
        cxx_bin = self.tmpdir / "k_cxx"
        ok, err = compile_c(clang_path, [str(cxx_src)], [], [], str(cxx_bin),
                            clang_cxx_path=clang_cxx_path)
        self.assertTrue(ok, f"C++ compile via configs/config.yaml paths failed: {err}")

    def test_registered_default_reproduces_optimize_py_call_shape(self):
        # optimize.py's main() calls set_default_cxx_compiler(config.compiler.
        # clang_cxx_path) once at startup, then every downstream call site
        # (extract_remarks_by_pass, get_ir_stats, _correctness_check, ...)
        # calls compile_c()/select_compiler() with ONLY `clang` in scope --
        # no clang_cxx_path argument at all. Reproduce that exact shape here:
        # register the real configured path, then compile a C++ file through
        # compile_c() passing clang_cxx_path=None, proving the registration
        # (not an explicit per-call argument) is what makes it resolve.
        clang_path, clang_cxx_path = self._load_configured_compiler_paths()
        self.addCleanup(set_default_cxx_compiler, get_default_cxx_compiler())
        set_default_cxx_compiler(clang_cxx_path)

        cxx_src = self.tmpdir / "k2.cpp"
        cxx_src.write_text(
            '#include <iostream>\n'
            'int main(){ std::cout << "registered-default-path" << std::endl; return 0; }\n')
        cxx_bin = self.tmpdir / "k2_cxx"
        ok, err = compile_c(clang_path, [str(cxx_src)], [], [], str(cxx_bin))  # no clang_cxx_path!
        self.assertTrue(ok, f"C++ compile via registered default failed: {err}")
        result = subprocess.run([str(cxx_bin)], capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "registered-default-path")


if __name__ == "__main__":
    unittest.main()
