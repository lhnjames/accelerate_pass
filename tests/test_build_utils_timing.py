import tempfile
import unittest
from pathlib import Path

from src.build_utils import run_timing


class RunTimingTests(unittest.TestCase):
    def test_timeout_is_configurable_for_large_kernels(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "sleepy"
            binary.write_text("#!/bin/sh\nsleep 0.05\n")
            binary.chmod(binary.stat().st_mode | 0o111)
            self.assertGreater(run_timing(str(binary), runs=1,
                                           timeout_seconds=1), 0.0)

    def test_timeout_failure_returns_negative_one(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "too_slow"
            binary.write_text("#!/bin/sh\nsleep 1\n")
            binary.chmod(binary.stat().st_mode | 0o111)
            self.assertEqual(run_timing(str(binary), runs=1,
                                        timeout_seconds=0.05), -1.0)


if __name__ == "__main__":
    unittest.main()
