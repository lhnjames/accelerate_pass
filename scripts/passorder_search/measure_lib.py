"""Compile+time helper for the AutoPass-style pass-order-search baseline.

Unlike the OpenCode harness (which reuses comet's compile_binary() as-is,
since it never touches pass order), this harness needs a bespoke pipeline:
the kernel function is frontend-compiled with ALL LLVM passes disabled
(-Xclang -disable-llvm-passes), then a custom pass order is applied to it
alone via `opt -passes=`, then it's codegen'd and linked against a normally
-O3-compiled utils/polybench.c. This isolates "does pass ORDER matter" from
"does the boilerplate/timer code get optimized" -- the latter is held
constant at -O3 for every condition in this ablation study.

No llvm-link is available on the deployed toolchain (only clang/clang++/
opt/llc were extracted from the .deb), so kernel and utils are optimized
and object-compiled separately and joined at the final `clang ... -o` link
step, rather than merged into one IR module before running opt. This also
keeps pass-order effects isolated to the actual computational kernel, which
is what a pass-order-search baseline should be measuring anyway.
"""
import subprocess
import sys
from pathlib import Path

COMET_ROOT = Path("/home/hanning/comet")
sys.path.insert(0, str(COMET_ROOT))

from src.build_utils import run_timing, compile_c   # noqa: E402
from src.correctness import detect_correctness_mode, check_correctness  # noqa: E402
import yaml  # noqa: E402

_cfg = yaml.safe_load((COMET_ROOT / "configs" / "config.yaml").read_text())
CLANG = _cfg["compiler"]["clang_path"]
OPT = _cfg["compiler"]["opt_path"]

STD_FLAGS = ["-std=gnu99"]


def compile_baseline(kernel_c: str, utils: str, source_dir: str, out_bin: str,
                      dataset: str = "LARGE_DATASET", runs: int = 3,
                      pin_cpu=None, timeout: int = 180):
    """Plain -O3 compile (both kernel + utils), used for the reference
    binary and the OC/comet-style baseline speedup denominator."""
    polybench_c = str(Path(utils) / "polybench.c")
    defines = [f"-D{dataset}", "-DPOLYBENCH_TIME"]
    ok, err = compile_c(CLANG, [kernel_c, polybench_c], [utils, source_dir],
                         defines, out_bin, timeout=timeout)
    if not ok:
        return False, -1.0, err
    ms = run_timing(out_bin, runs=runs, pin_cpu=pin_cpu)
    if ms <= 0:
        return False, -1.0, "run_timing returned <= 0 (crash or timeout)"
    return True, ms, ""


def _run(cmd, timeout=180):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
    except Exception as e:
        return False, f"exception running {cmd[0]}: {e}"
    if r.returncode != 0:
        return False, (r.stderr or r.stdout)[-2000:]
    return True, ""


def compile_with_pass_order(kernel_c: str, utils: str, source_dir: str,
                             pass_order: list, out_bin: str,
                             dataset: str = "LARGE_DATASET",
                             work_dir: "str | None" = None, timeout: int = 180):
    """Compile kernel_c with a CUSTOM pass order (mem2reg always first),
    utils/polybench.c at plain -O3, link together. Returns (ok, err)."""
    wd = Path(work_dir) if work_dir else Path(kernel_c).parent
    inc = [f"-I{utils}", f"-I{source_dir}"]
    defines = [f"-D{dataset}", "-DPOLYBENCH_TIME"]

    kernel_raw_ll = wd / "kernel_raw.ll"
    kernel_opt_ll = wd / "kernel_opt.ll"
    kernel_opt_o = wd / "kernel_opt.o"
    utils_o = wd / "polybench_o3.o"
    polybench_c = str(Path(utils) / "polybench.c")

    # 1. Frontend-compile the kernel with all LLVM passes disabled -- gives
    #    canonical (alloca-based, non-SSA) IR, same shape as clang -O0 output.
    ok, err = _run([CLANG, "-O1", "-Xclang", "-disable-llvm-passes"] + STD_FLAGS
                    + inc + defines + ["-S", "-emit-llvm", kernel_c,
                                        "-o", str(kernel_raw_ll)], timeout=timeout)
    if not ok:
        return False, f"frontend IR emit failed: {err}"

    # 2. Apply the candidate pass order (mem2reg first, unconditionally --
    #    without it every later pass sees the same alloca-heavy IR as -O0).
    passes_str = "mem2reg," + ",".join(pass_order) if pass_order else "mem2reg"
    ok, err = _run([OPT, f"-passes={passes_str}", "-S",
                    str(kernel_raw_ll), "-o", str(kernel_opt_ll)], timeout=timeout)
    if not ok:
        return False, f"opt -passes={passes_str} failed: {err}"

    # 3. Codegen the optimized IR to an object file (no further clang-level
    #    optimization passes -- default is -O0 codegen when unspecified).
    ok, err = _run([CLANG, "-c", str(kernel_opt_ll), "-o", str(kernel_opt_o)],
                   timeout=timeout)
    if not ok:
        return False, f"kernel_opt.ll codegen failed: {err}"

    # 4. utils/polybench.c compiled normally at -O3 (held constant across
    #    every condition in this ablation study -- only the kernel's pass
    #    order varies here).
    ok, err = _run([CLANG, "-O3"] + STD_FLAGS + inc + defines
                    + ["-c", polybench_c, "-o", str(utils_o)], timeout=timeout)
    if not ok:
        return False, f"utils -O3 compile failed: {err}"

    # 5. Link.
    ok, err = _run([CLANG, str(kernel_opt_o), str(utils_o), "-o", out_bin, "-lm"],
                   timeout=timeout)
    if not ok:
        return False, f"link failed: {err}"
    return True, ""


def time_binary(out_bin: str, runs: int = 1, pin_cpu=None):
    ms = run_timing(out_bin, runs=runs, pin_cpu=pin_cpu)
    if ms <= 0:
        return False, -1.0
    return True, ms


def correctness_check(ref_bin: str, opt_bin: str, mode: str):
    return check_correctness(ref_bin, opt_bin, mode)
