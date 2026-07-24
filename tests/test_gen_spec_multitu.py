"""
Tests for scripts/gen_spec_multitu.py -- the non-unity, multi-TU manifest
generator for C++ SPEC benchmarks (Phase 1: namd_r only).

Uses a small FAKE/synthetic SPEC tree (tiny stub .C/.h files under the real
508.namd_r file NAMES, not the real ~8000-line NAMD source) built in a temp
directory and pointed at via the SPEC_CPU_ROOT env var -- this deliberately
never touches the real remote SPEC install, never compiles, and never times
anything. It verifies generation-time correctness only:
  - the 15-source list (entry + 14 support files) is exactly right
  - every generated source is tagged/compiled as C++ (via is_cxx_source)
  - cxx_standard is gnu++03 (not some default)
  - both the "test" and "ref" runtime contracts appear with resolved
    absolute-path argv (no rundir/chdir placeholders left over)
  - paths honor SPEC_CPU_ROOT
  - no unity concatenation happened: every non-entry file's copy is BYTE-
    IDENTICAL to the fake source (entry file is the only one modified, and
    only by renaming main + appending the wrapper -- its original body is
    still present unchanged)
  - the emitted build_manifest.json actually parses via
    src/build_manifest.py::load_build_manifest (schema-level sanity, not a
    build)

Run with:
    python3 -m unittest tests.test_gen_spec_multitu -v
from the repo root (comet/).
"""
import importlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

from src.build_utils import is_cxx_source
from src.build_manifest import load_build_manifest

# The 14 real support-file names from Spec/object.pm's @sources (minus the
# entry file spec_namd.C) -- gen_spec_multitu.BENCHMARKS hardcodes these
# real names (same design as gen_spec_kernels.py's own BENCHMARKS dict), so
# the fake tree must provide stubs under these exact names.
NAMD_SUPPORT_FILES = [
    "Compute.C", "ComputeList.C", "ComputeNonbondedFEP.C",
    "ComputeNonbondedLES.C", "ComputeNonbondedPProf.C",
    "ComputeNonbondedStd.C", "ComputeNonbondedUtil.C",
    "LJTable.C", "Molecule.C", "Patch.C", "PatchList.C",
    "ResultSet.C", "SimParameters.C", "erf.C",
]
DEEPSJENG_SOURCES = [
    "attacks.cpp", "bitboard.cpp", "bits.cpp", "board.cpp", "draw.cpp",
    "endgame.cpp", "epd.cpp", "generate.cpp", "initp.cpp", "make.cpp",
    "moves.cpp", "neval.cpp", "pawn.cpp", "preproc.cpp", "search.cpp",
    "see.cpp", "sjeng.cpp", "state.cpp", "ttable.cpp", "utils.cpp",
]
LEELA_SOURCES = [
    "FullBoard.cpp", "KoState.cpp", "Playout.cpp", "TimeControl.cpp",
    "UCTSearch.cpp", "GameState.cpp", "Leela.cpp", "SGFParser.cpp",
    "Timing.cpp", "Utils.cpp", "FastBoard.cpp", "Matcher.cpp",
    "SGFTree.cpp", "TTable.cpp", "Zobrist.cpp", "FastState.cpp", "GTP.cpp",
    "MCOTable.cpp", "Random.cpp", "SMP.cpp", "UCTNode.cpp",
]

_FAKE_ENTRY_SRC = (
    "// fake stand-in for spec_namd.C -- not real NAMD source\n"
    "#include <cstdio>\n"
    "int main(int argc, char **argv) {\n"
    "  printf(\"fake namd entry, argc=%d\\n\", argc);\n"
    "  return 0;\n"
    "}\n"
)


class TestGenSpecMultiTU(unittest.TestCase):
    def setUp(self):
        self.fake_root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.fake_root, ignore_errors=True)
        self._build_fake_spec_tree()

        # Reload gen_spec_multitu AFTER SPEC_CPU_ROOT is set, since it reads
        # the env var at import time (module-level SPEC_ROOT constant) --
        # same pattern gen_spec_kernels.py itself uses.
        self._old_env = os.environ.get("SPEC_CPU_ROOT")
        os.environ["SPEC_CPU_ROOT"] = str(self.fake_root)
        self.addCleanup(self._restore_env)

        import gen_spec_multitu
        importlib.reload(gen_spec_multitu)
        self.gen = gen_spec_multitu

        # Redirect generated output into the temp tree too, so a test run
        # never writes into the real repo's SPEC_multitu_root/.
        self.out_root = self.fake_root / "_out"
        self.gen.OUT_ROOT = self.out_root

    def _restore_env(self):
        if self._old_env is None:
            os.environ.pop("SPEC_CPU_ROOT", None)
        else:
            os.environ["SPEC_CPU_ROOT"] = self._old_env

    def _build_fake_spec_tree(self):
        bench_dir = self.fake_root / "508.namd_r"
        src_dir = bench_dir / "src"
        src_dir.mkdir(parents=True)

        (src_dir / "spec_namd.C").write_text(_FAKE_ENTRY_SRC)
        for fname in NAMD_SUPPORT_FILES:
            (src_dir / fname).write_text(
                f"// fake stand-in for {fname} -- not real NAMD source\n"
                f"int {Path(fname).stem}_marker(void) {{ return 0; }}\n")
        # A couple of fake headers, just to exercise header flattening.
        (src_dir / "common.h").write_text("#ifndef COMMON_H\n#define COMMON_H\n#endif\n")
        (src_dir / "NamdTypes.h").write_text("#ifndef NAMDTYPES_H\n#define NAMDTYPES_H\n#endif\n")

        data_all = bench_dir / "data" / "all" / "input"
        data_all.mkdir(parents=True)
        (data_all / "apoa1.input").write_text("fake molecule data, not real apoa1.input\n")

        self._build_flat_cpp_benchmark(
            "531.deepsjeng_r", DEEPSJENG_SOURCES, "sjeng.cpp",
            {"test": "test.txt", "train": "train.txt", "refrate": "ref.txt"})
        self._build_flat_cpp_benchmark(
            "541.leela_r", LEELA_SOURCES, "Leela.cpp",
            {"test": "test.sgf", "train": "train.sgf", "refrate": "ref.sgf"})
        leela_src = self.fake_root / "541.leela_r" / "src"
        (leela_src / "boost" / "preprocessor" / "detail").mkdir(parents=True)
        (leela_src / "boost" / "mpl" / "aux_").mkdir(parents=True)
        (leela_src / "boost" / "preprocessor" / "detail" / "foo.hpp").write_text(
            "// preprocessor foo\n")
        (leela_src / "boost" / "mpl" / "aux_" / "foo.hpp").write_text(
            "// mpl foo with same basename\n")

    def _build_flat_cpp_benchmark(self, bench, sources, entry, workloads):
        src_dir = self.fake_root / bench / "src"
        src_dir.mkdir(parents=True)
        for source in sources:
            text = ("int main(int argc, char **argv) { return argc < 1; }\n"
                    if source == entry else f"// fake {source}\n")
            (src_dir / source).write_text(text)
        (src_dir / "common.h").write_text("// common header\n")
        for workload, filename in workloads.items():
            input_dir = self.fake_root / bench / "data" / workload / "input"
            input_dir.mkdir(parents=True)
            (input_dir / filename).write_text(f"fake {workload} input\n")

    # ── source list / language / standard ─────────────────────────────────

    def test_generates_manifest_without_error(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        self.assertTrue(manifest_path.exists())

    def test_source_list_has_entry_plus_14_support_files(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        self.assertEqual(len(data["sources"]), 15,
                         f"expected entry + 14 support files, got: {data['sources']}")
        basenames = {Path(s).name for s in data["sources"]}
        self.assertIn("spec_namd.C", basenames)
        for fname in NAMD_SUPPORT_FILES:
            self.assertIn(fname, basenames)

    def test_all_sources_are_cxx(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        for s in data["sources"]:
            self.assertTrue(is_cxx_source(s), f"{s} not recognized as C++ (wrong extension?)")

    def test_cxx_standard_is_gnu_plus_plus_03(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        self.assertEqual(data["cxx_standard"], "gnu++03")

    def test_defines_include_spec_common_and_benchmark_specific(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        for d in ("SPEC", "NDEBUG", "SPEC_LP64", "NAMD_DISABLE_SSE",
                 "SPEC_AUTO_SUPPRESS_OPENMP"):
            self.assertIn(d, data["defines"])

    # ── argv / runtime contract ────────────────────────────────────────────

    def test_runtime_and_workloads_both_present(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        self.assertIn("runtime", data)
        self.assertIn("workloads", data)
        self.assertIn("test", data["workloads"])
        self.assertIn("ref", data["workloads"])

    def test_test_workload_argv_resolved_absolute_and_correct_iterations(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        argv = data["workloads"]["test"]["argv"]
        self.assertEqual(argv[0], "kernel_namd_r")
        self.assertIn("--input", argv)
        input_path = argv[argv.index("--input") + 1]
        self.assertTrue(Path(input_path).is_absolute(),
                        f"--input path not resolved to absolute: {input_path}")
        self.assertTrue(Path(input_path).exists(),
                        f"resolved --input path doesn't exist: {input_path}")
        self.assertIn("--iterations", argv)
        self.assertEqual(argv[argv.index("--iterations") + 1], "1")
        self.assertIn("--output", argv)

    def test_ref_workload_argv_has_65_iterations(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        argv = data["workloads"]["ref"]["argv"]
        self.assertEqual(argv[argv.index("--iterations") + 1], "65")

    def test_test_and_ref_share_the_same_input_file(self):
        # Both workloads read the same shared apoa1.input under data/all/input/
        # (SPEC's convention: files common across workload sizes live in
        # all/) -- not two different files.
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        test_argv = data["workloads"]["test"]["argv"]
        ref_argv = data["workloads"]["ref"]["argv"]
        test_input = test_argv[test_argv.index("--input") + 1]
        ref_input = ref_argv[ref_argv.index("--input") + 1]
        self.assertEqual(test_input, ref_input)
        self.assertTrue(test_input.endswith("apoa1.input"))

    def test_no_rundir_or_chdir_needed(self):
        # namd_r's spec_namd.C fopen()s its argv values directly -- unlike
        # nab_r, no "rundir" staging / chdir() should appear anywhere.
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        data = json.loads(manifest_path.read_text())
        self.assertNotIn("rundir", data)
        entry_out = manifest_path.parent / "sources" / "spec_namd.C"
        self.assertNotIn("chdir", entry_out.read_text())

    # ── paths honor SPEC_CPU_ROOT ──────────────────────────────────────────

    def test_reads_from_spec_cpu_root_env_var(self):
        # setUp already pointed SPEC_CPU_ROOT at the fake tree; a successful
        # generation (checked elsewhere) IS the proof the module reads that
        # var. Confirm the module's own resolved constant matches too.
        self.assertEqual(self.gen.SPEC_ROOT, self.fake_root)

    # ── no unity concatenation ──────────────────────────────────────────────

    def test_support_files_copied_byte_identical_no_concatenation(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        sources_dir = manifest_path.parent / "sources"
        src_root = self.fake_root / "508.namd_r" / "src"
        for fname in NAMD_SUPPORT_FILES:
            original = (src_root / fname).read_text()
            copy = (sources_dir / fname).read_text()
            self.assertEqual(copy, original,
                             f"{fname} was modified during generation -- support "
                             f"files must be copied as-is, never concatenated/edited")

    def test_support_files_are_separate_files_not_one_blob(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        sources_dir = manifest_path.parent / "sources"
        # If this were unity-built, there'd be one combined file (e.g.
        # "polybench.c"/"utils.c") instead of each support file existing
        # separately on disk.
        for fname in NAMD_SUPPORT_FILES:
            self.assertTrue((sources_dir / fname).is_file(),
                           f"{fname} missing as its own separate file")
        self.assertFalse((sources_dir / "polybench.c").exists())
        self.assertFalse((sources_dir / "utils.c").exists())

    def test_entry_file_has_main_renamed_but_body_otherwise_intact(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        entry_out = (manifest_path.parent / "sources" / "spec_namd.C").read_text()
        self.assertIn("kernel_namd_r(int argc, char **argv)", entry_out)
        self.assertIn('printf("fake namd entry, argc=%d\\n", argc);', entry_out,
                     "original entry body should survive unmodified aside from the rename")
        # A new, separate main() must be appended (the wrapper), not a
        # rewrite of the original one.
        self.assertEqual(entry_out.count("int main("), 1)

    # ── manifest loads via the real loader (schema-level integration) ─────

    def test_manifest_parses_via_load_build_manifest(self):
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(err, err)
        manifest = load_build_manifest(manifest_path)
        self.assertEqual(manifest.name, "namd_r")
        self.assertTrue(manifest.uses_cxx)
        self.assertEqual(len(manifest.sources), 15)
        self.assertEqual(manifest.cxx_standard, "gnu++03")
        self.assertEqual(manifest.default_workload, "test")
        self.assertEqual(manifest.runtime_for("test").argv[-1],
                         str(manifest_path.parent / "run" / "apoa1.test.output"))
        self.assertIn("65", manifest.runtime_for("ref").argv)
        for unit in manifest.sources:
            self.assertTrue(unit.path.is_file(), f"manifest source doesn't resolve: {unit.path}")

    # ── error handling ──────────────────────────────────────────────────────

    def test_missing_entry_file_reports_error_not_exception(self):
        (self.fake_root / "508.namd_r" / "src" / "spec_namd.C").unlink()
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(manifest_path)
        self.assertIn("spec_namd.C", err)

    def test_missing_support_file_reports_error_not_exception(self):
        (self.fake_root / "508.namd_r" / "src" / "Molecule.C").unlink()
        manifest_path, err = self.gen.gen_one("namd_r", self.gen.BENCHMARKS["namd_r"])
        self.assertIsNone(manifest_path)
        self.assertIn("Molecule.C", err)

    def test_deepsjeng_manifest_has_20_tus_memory_define_and_ref_input(self):
        path, err = self.gen.gen_one(
            "deepsjeng_r", self.gen.BENCHMARKS["deepsjeng_r"])
        self.assertIsNone(err, err)
        manifest = load_build_manifest(path)
        self.assertEqual(len(manifest.sources), 20)
        self.assertEqual(manifest.cxx_standard, "gnu++03")
        self.assertIn("SMALL_MEMORY", manifest.defines)
        self.assertTrue(manifest.runtime_for("ref").argv[-1].endswith("ref.txt"))
        self.assertTrue(Path(manifest.runtime_for("ref").argv[-1]).is_file())

    def test_leela_preserves_recursive_boost_tree_and_duplicate_basenames(self):
        path, err = self.gen.gen_one("leela_r", self.gen.BENCHMARKS["leela_r"])
        self.assertIsNone(err, err)
        manifest = load_build_manifest(path)
        self.assertEqual(len(manifest.sources), 21)
        self.assertEqual(manifest.cxx_standard, "gnu++03")
        source_root = path.parent / "sources"
        first = source_root / "boost" / "preprocessor" / "detail" / "foo.hpp"
        second = source_root / "boost" / "mpl" / "aux_" / "foo.hpp"
        self.assertEqual(first.read_text(), "// preprocessor foo\n")
        self.assertEqual(second.read_text(), "// mpl foo with same basename\n")

    def test_missing_recursive_header_tree_is_reported(self):
        path, err = self.gen.gen_one("leela_r", self.gen.BENCHMARKS["leela_r"])
        self.assertIsNone(err, err)
        self.assertTrue(path.is_file())
        shutil.rmtree(self.fake_root / "541.leela_r" / "src" / "boost")
        path, err = self.gen.gen_one("leela_r", self.gen.BENCHMARKS["leela_r"])
        self.assertIsNone(path)
        self.assertIn("header tree not found", err)
        self.assertFalse(
            (self.out_root / "leela_r" / "build_manifest.json").exists(),
            "failed regeneration must invalidate a stale prior manifest")


if __name__ == "__main__":
    unittest.main()
