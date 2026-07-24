import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from src.build_manifest import MultiTUBuilder, load_build_manifest
from src.config import ConfigLoader


class TestMultiTUBuilder(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        config_root = Path(__file__).resolve().parents[1] / "configs"
        self.config = ConfigLoader(str(config_root)).load_all()

    def _write_manifest(self, payload):
        path = self.root / "build.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return load_build_manifest(path)

    def test_mixed_c_cxx_translation_units_compile_separately(self):
        (self.root / "helper.c").write_text("int helper(void) { return 40; }\n")
        (self.root / "main.cpp").write_text(
            '#include <iostream>\nextern "C" int helper(void);\n'
            'int main() { std::cout << helper() + 2 << "\\n"; }\n')
        manifest = self._write_manifest({
            "version": 1,
            "name": "mixed",
            "sources": ["helper.c", "main.cpp"],
        })
        self.assertTrue(manifest.uses_cxx)
        result = MultiTUBuilder(self.config.compiler).build(
            manifest, self.root / "mixed", self.root / "objects")
        self.assertTrue(result.success, result.error)
        self.assertEqual(subprocess.check_output([result.binary], text=True), "42\n")
        compile_commands = result.commands[:-1]
        self.assertEqual(len(compile_commands), 2)
        self.assertEqual(compile_commands[0][0], self.config.compiler.clang_path)
        self.assertEqual(compile_commands[1][0], self.config.compiler.clang_cxx_path)
        self.assertEqual(result.commands[-1][0], self.config.compiler.clang_cxx_path)

    def test_per_translation_unit_flags_are_preserved(self):
        (self.root / "main.c").write_text(
            "#ifndef VALUE\n#error missing VALUE\n#endif\nint main(void){return VALUE != 7;}\n")
        manifest = self._write_manifest({
            "version": 1,
            "name": "flags",
            "sources": [{"path": "main.c", "flags": ["-DVALUE=7"]}],
        })
        result = MultiTUBuilder(self.config.compiler).build(
            manifest, self.root / "flags", self.root / "objects")
        self.assertTrue(result.success, result.error)
        self.assertEqual(subprocess.run([result.binary]).returncode, 0)

    def test_missing_source_fails_with_specific_path(self):
        manifest = self._write_manifest({
            "version": 1, "name": "missing", "sources": ["missing.cpp"]})
        result = MultiTUBuilder(self.config.compiler).build(
            manifest, self.root / "out", self.root / "objects")
        self.assertFalse(result.success)
        self.assertIn("missing.cpp", result.error)

    def test_manifest_requires_version_and_sources(self):
        with self.assertRaises(ValueError):
            self._write_manifest({"version": 2, "name": "bad", "sources": ["a.c"]})
        with self.assertRaises(ValueError):
            self._write_manifest({"version": 1, "name": "bad", "sources": []})

    def test_named_workloads_are_loaded_and_selectable(self):
        (self.root / "main.c").write_text("int main(void){return 0;}\n")
        manifest = self._write_manifest({
            "version": 1,
            "name": "workloads",
            "sources": ["main.c"],
            "default_workload": "test",
            "workloads": {
                "test": {"cwd": "test-run", "argv": ["program", "1"]},
                "ref": {"cwd": "ref-run", "argv": ["program", "65"]},
            },
        })
        self.assertEqual(manifest.runtime_for().argv, ("program", "1"))
        self.assertEqual(manifest.runtime_for("ref").argv, ("program", "65"))
        self.assertTrue(manifest.runtime_for("ref").cwd.is_absolute())
        with self.assertRaisesRegex(ValueError, "unknown workload"):
            manifest.runtime_for("train")

    def test_runtime_requires_logical_argv_zero(self):
        (self.root / "main.c").write_text("int main(void){return 0;}\n")
        with self.assertRaisesRegex(ValueError, "argv"):
            self._write_manifest({
                "version": 1, "name": "bad-runtime", "sources": ["main.c"],
                "runtime": {"cwd": ".", "argv": []},
            })

    def test_default_workload_must_exist(self):
        (self.root / "main.c").write_text("int main(void){return 0;}\n")
        with self.assertRaisesRegex(ValueError, "default_workload"):
            self._write_manifest({
                "version": 1, "name": "bad-default", "sources": ["main.c"],
                "default_workload": "ref",
                "workloads": {"test": {"argv": ["program"]}},
            })


if __name__ == "__main__":
    unittest.main()
