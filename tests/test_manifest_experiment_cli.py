import tempfile
import unittest
from pathlib import Path

from scripts.run_manifest_experiment import _atomic_json, make_parser


class ManifestExperimentCliTests(unittest.TestCase):
    def test_candidate_flags_preserve_mllvm_pair(self):
        args = make_parser().parse_args([
            "manifest.json", "--result-dir", "result",
            "--candidate-flag=-mllvm",
            "--candidate-flag=--licm-max-num-uses-traversed=32",
            "--workload", "ref", "--pairs", "9", "--pin-cpu", "3",
        ])
        self.assertEqual(args.candidate_flag, [
            "-mllvm", "--licm-max-num-uses-traversed=32"])
        self.assertEqual(args.workload, "ref")
        self.assertEqual(args.pairs, 9)
        self.assertEqual(args.pin_cpu, 3)

    def test_atomic_json_replaces_temporary_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "experiment.json"
            _atomic_json(path, {"status": "ok"})
            self.assertIn('"status": "ok"', path.read_text())
            self.assertEqual(list(path.parent.glob(".*.tmp-*")), [])


if __name__ == "__main__":
    unittest.main()
