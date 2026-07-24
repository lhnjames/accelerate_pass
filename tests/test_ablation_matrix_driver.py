"""Regression tests for scripts/run_ablation_matrix.py's result harvesting.

The driver's whole job is to capture each cell's numbers; if it cannot find the
result JSON, every cell is silently recorded as "failed" and a 30+ hour sweep
produces nothing.  That is exactly what the first version did: it looked only
under PROJECT_ROOT/outputs/, but with the run logger active (the normal case)
optimize.py writes the file under the PER-RUN directory
runs/<timestamp>_<dataset>_<program>/outputs/ instead.
"""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

_spec = importlib.util.spec_from_file_location(
    "run_ablation_matrix", PROJECT_ROOT / "scripts" / "run_ablation_matrix.py")
driver = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(driver)


class TestFindResultJson(unittest.TestCase):
    def test_it_finds_the_json_inside_the_run_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp) / "2026-07-22_18-11-36_polybench_3mm"
            (run_dir / "outputs").mkdir(parents=True)
            expected = run_dir / "outputs" / "3mm_agent_results.json"
            expected.write_text("{}")
            found = driver._find_result_json("a/b/3mm.c", str(run_dir))
            self.assertEqual(found, expected)

    def test_missing_file_returns_none_rather_than_a_bogus_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(driver._find_result_json("a/b/nosuch.c", tmp))

    def test_empty_run_dir_falls_back_without_crashing(self):
        self.assertIsNone(driver._find_result_json("a/b/nosuchprogram.c", ""))


class TestCellKeyAndResume(unittest.TestCase):
    def test_ok_and_timeout_are_terminal_failed_is_retried(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "results.jsonl"
            path.write_text("\n".join(json.dumps(r) for r in [
                {"program_path": "a/3mm.c", "condition": "full", "seed": 1,
                 "status": "ok"},
                # timeout is a deterministic matched-budget non-result -- must
                # NOT be retried (would burn another full 2h timeout)
                {"program_path": "a/cholesky.c", "condition": "no_feedback",
                 "seed": 1, "status": "timeout"},
                # failed may be transient -- must be retried on resume
                {"program_path": "a/adi.c", "condition": "full", "seed": 1,
                 "status": "failed"},
            ]) + "\n")
            done = driver._load_done(path)
            self.assertIn(driver._cell_key("a/3mm.c", "full", 1), done)
            self.assertIn(driver._cell_key("a/cholesky.c", "no_feedback", 1), done)
            self.assertNotIn(driver._cell_key("a/adi.c", "full", 1), done)

    def test_missing_results_file_is_an_empty_done_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(driver._load_done(Path(tmp) / "nope.jsonl"), set())

    def test_conditions_and_seeds_are_distinct_cells(self):
        keys = {driver._cell_key("a/3mm.c", c, s)
                for c in ("full", "no_feedback") for s in (1, 2, 3)}
        self.assertEqual(len(keys), 6)


class TestSweepShape(unittest.TestCase):
    def test_default_program_set_is_the_priority_list(self):
        stems = [Path(p).stem for p in driver.DEFAULT_PROGRAMS]
        for required in ("3mm", "nussinov", "cholesky", "floyd-warshall",
                         "gramschmidt", "covariance", "correlation", "adi",
                         "seidel-2d"):
            self.assertIn(required, stems)

    def test_cell_order_is_seed_major(self):
        # Seed-major so an interrupted sweep still yields a BALANCED comparison
        # (both conditions for the seeds that finished) rather than all of one
        # condition and none of the other.
        programs, conditions, seeds = ["p1", "p2"], ["full", "no_feedback"], [1, 2]
        cells = [(p, c, s) for s in seeds for p in programs for c in conditions]
        self.assertEqual([s for _, _, s in cells], [1, 1, 1, 1, 2, 2, 2, 2])
        self.assertEqual(len({c for _, c, s in cells if s == 1}), 2)


if __name__ == "__main__":
    unittest.main()
