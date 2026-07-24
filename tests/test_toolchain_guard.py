import stat
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from src.toolchain_guard import parse_llvm_major, verify_llvm21_toolchain
from src.run_logger import RunLogger


class ToolchainGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _tool(self, name, version="21.1.8"):
        path = self.root / name
        path.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' 'Ubuntu LLVM version {version}'\n",
            encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def _config(self, version="21.1.8"):
        return SimpleNamespace(
            clang_path=self._tool("clang-21", version),
            clang_cxx_path=self._tool("clang++-21", version),
            opt_path=self._tool("opt-21", version),
            llc_path=self._tool("llc-21", version),
        )

    def test_parses_clang_and_llvm_version_formats(self):
        self.assertEqual(parse_llvm_major("Ubuntu clang version 21.1.5"), 21)
        self.assertEqual(parse_llvm_major("Ubuntu LLVM version 21.1.8"), 21)

    def test_validates_all_four_tools_and_returns_stable_hash(self):
        config = self._config()
        first = verify_llvm21_toolchain(config)
        second = verify_llvm21_toolchain(config)
        self.assertEqual([tool.role for tool in first.tools],
                         ["clang", "clang++", "opt", "llc"])
        self.assertTrue(all(tool.major == 21 for tool in first.tools))
        self.assertEqual(first.identity_sha256, second.identity_sha256)

    def test_rejects_any_non_21_component(self):
        config = self._config("20.1.0")
        with self.assertRaisesRegex(RuntimeError, "requires LLVM 21"):
            verify_llvm21_toolchain(config)

    def test_rejects_missing_tool(self):
        config = self._config()
        config.opt_path = str(self.root / "missing-opt-21")
        with self.assertRaisesRegex(RuntimeError, "missing"):
            verify_llvm21_toolchain(config)


class RunLoggerMetadataTests(unittest.TestCase):
    def test_update_meta_preserves_initial_fields(self):
        import json
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as temporary:
            with patch("src.run_logger.RUNS_ROOT", Path(temporary)):
                logger = RunLogger("kernel", "test")
                try:
                    logger.update_meta({"toolchain": {"identity_sha256": "abc"}})
                    meta = json.loads((logger.run_dir / "meta.json").read_text())
                    self.assertEqual(meta["program"], "kernel")
                    self.assertEqual(meta["toolchain"]["identity_sha256"], "abc")
                finally:
                    logger.close()


if __name__ == "__main__":
    unittest.main()
