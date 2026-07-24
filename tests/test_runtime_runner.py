import json
import stat
import tempfile
import unittest
from pathlib import Path

from src.build_manifest import load_build_manifest
from src.runtime_runner import (
    RuntimeExecutor, compare_runtime_correctness,
    detect_runtime_correctness_mode, measure_paired_runtime,
)


class RuntimeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.run_dir = self.root / "run"

    def tearDown(self):
        self.tmp.cleanup()

    def _script(self, name, body):
        path = self.root / name
        path.write_text("#!/bin/sh\nset -eu\n" + body, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _contract(self, argv, output_files=()):
        source = self.root / "dummy.c"
        source.write_text("int main(void){return 0;}\n")
        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(json.dumps({
            "version": 1, "name": "runtime", "sources": ["dummy.c"],
            "runtime": {
                "cwd": str(self.run_dir), "argv": argv,
                "output_files": [str(path) for path in output_files],
            },
        }), encoding="utf-8")
        return load_build_manifest(manifest_path).runtime_for()

    def test_passes_manifest_arguments_and_captures_declared_product(self):
        product = self.run_dir / "result.txt"
        script = self._script("write-result", 'printf "%s\\n" "$1" > "$2"\n')
        contract = self._contract(
            ["logical-name", "42", str(product)], [product])
        observation = RuntimeExecutor(script, contract).run()
        self.assertEqual(observation.returncode, 0)
        self.assertEqual(observation.output_files[0][1], b"42\n")

    def test_numeric_correctness_uses_declared_output_not_console_noise(self):
        ref_product = self.run_dir / "ref.txt"
        candidate_product = self.run_dir / "candidate.txt"
        reference_script = self._script(
            "reference", 'echo noisy-ref >&2; printf "1.0 2.0\\n" > "$1"\n')
        candidate_script = self._script(
            "candidate", 'echo different-noise >&2; printf "1.0 2.00001\\n" > "$1"\n')
        ref_executor = RuntimeExecutor(
            reference_script,
            self._contract(["ref", str(ref_product)], [ref_product]))
        candidate_executor = RuntimeExecutor(
            candidate_script,
            self._contract(["candidate", str(candidate_product)], [candidate_product]))
        mode, ref_observation = detect_runtime_correctness_mode(ref_executor)
        self.assertEqual(mode, "numeric")
        result = compare_runtime_correctness(
            ref_observation, candidate_executor.run(), mode, epsilon=1e-4)
        self.assertTrue(result.ok, result.message)

    def test_hash_correctness_detects_binary_mismatch(self):
        product = self.run_dir / "product.bin"
        first = self._script("first", 'printf "\\001\\002" > "$1"\n')
        second = self._script("second", 'printf "\\001\\003" > "$1"\n')
        contract = self._contract(["program", str(product)], [product])
        first_executor = RuntimeExecutor(first, contract)
        mode, reference = detect_runtime_correctness_mode(first_executor)
        self.assertEqual(mode, "hash")
        result = compare_runtime_correctness(
            reference, RuntimeExecutor(second, contract).run(), mode)
        self.assertFalse(result.ok)
        self.assertIn("hash mismatch", result.message)

    def test_rejects_output_path_outside_runtime_directory(self):
        script = self._script("noop", "exit 0\n")
        outside = self.root / "outside.txt"
        contract = self._contract(["program"], [outside])
        with self.assertRaisesRegex(ValueError, "outside runtime cwd"):
            RuntimeExecutor(script, contract).run()

    def test_paired_measurement_returns_raw_balanced_samples(self):
        script = self._script("fast", "exit 0\n")
        contract = self._contract(["program"])
        reference = RuntimeExecutor(script, contract)
        candidate = RuntimeExecutor(script, contract)
        summary = measure_paired_runtime(
            reference, candidate, pairs=3, bootstrap_resamples=50,
            bootstrap_seed=503)
        self.assertEqual(summary.sample_count, 3)
        self.assertEqual(len(summary.baseline_seconds), 3)
        self.assertEqual(len(summary.candidate_seconds), 3)


if __name__ == "__main__":
    unittest.main()
