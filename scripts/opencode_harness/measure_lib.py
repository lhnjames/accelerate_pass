"""Shared compile+time helper for the OpenCode baseline, reusing comet's own
compile_binary/run_timing so speedups are measured identically to conditions
(1)/(2)/(3). Import-only module; see measure_cli.py for the CLI entrypoint
opencode calls from inside its own scratch dir.
"""
import sys
from pathlib import Path

COMET_ROOT = Path("/home/hanning/comet")
sys.path.insert(0, str(COMET_ROOT))

from src.build_utils import run_timing, select_compiler          # noqa: E402
from tune_param import compile_binary                             # noqa: E402
from src.correctness import detect_correctness_mode, check_correctness  # noqa: E402
import yaml                                                        # noqa: E402

_cfg = yaml.safe_load((COMET_ROOT / "configs" / "config.yaml").read_text())
CLANG = _cfg["compiler"]["clang_path"]


def compile_and_time(kernel_c: str, utils: str, source_dir: str, runs: int = 1,
                      dataset: str = "LARGE_DATASET", out_bin: str = "/tmp/oc_bin",
                      extra_flags=None):
    """Compile kernel_c against utils/polybench.c and time it. Returns
    (ok: bool, ms: float, err: str)."""
    polybench_c = Path(utils) / "polybench.c"
    ok, err = compile_binary(CLANG, kernel_c, polybench_c, Path(utils), Path(source_dir),
                              Path(out_bin), extra_flags=extra_flags, dataset=dataset)
    if not ok:
        return False, -1.0, err
    ms = run_timing(out_bin, runs=runs)
    if ms <= 0:
        return False, -1.0, "run_timing returned <= 0 (crash or timeout)"
    return True, ms, ""


def correctness_check(ref_bin: str, opt_bin: str, mode: str):
    return check_correctness(ref_bin, opt_bin, mode)
