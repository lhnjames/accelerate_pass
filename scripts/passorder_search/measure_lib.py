"""Compile+time helper for the AutoPass-style pass-order-search baseline.

Unlike the OpenCode harness (which reuses comet's compile_binary() as-is,
since it never touches pass order), this harness needs a bespoke pipeline:
the kernel function is frontend-compiled with ALL LLVM passes disabled
(-Xclang -disable-llvm-passes), then a custom pass order is applied to it
alone via `opt -passes=`, then it's codegen'd and linked against a normally
-O3-compiled utils/polybench.c. This isolates "does pass ORDER matter" from
"does the boilerplate/timer code get optimized" -- the latter is held
constant at -O3 for every condition in this ablation study.

Kernel and utils are optimized and object-compiled separately and joined at
the final `clang ... -o` link step, rather than merged into one IR module
before running opt. This keeps pass-order effects isolated to the actual
computational kernel, which is what a pass-order-search baseline should be
measuring anyway. (An earlier version of this note claimed llvm-link was
unavailable on the deployed toolchain; it is in fact present as llvm-link-21
alongside clang/clang++/opt/llc. Separate compilation is a deliberate choice
here, not a toolchain limitation.)
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
LLC = _cfg["compiler"]["llc_path"]

STD_FLAGS = ["-std=gnu99"]


def compile_baseline(kernel_c: str, utils: str, source_dir: str, out_bin: str,
                      dataset: str = "LARGE_DATASET", runs: int = 3,
                      pin_cpu=None, timeout: int = 180):
    """Plain -O3 compile (both kernel + utils), used for the reference
    binary and the OC/comet-style baseline speedup denominator.

    Built with -DPOLYBENCH_TIME -- for TIMING only. Do not also use this
    binary's stdout for correctness comparison: under POLYBENCH_TIME,
    polybench_print_instruments expands to polybench_timer_print(), which
    prints only the elapsed wall-clock time, not the computed array
    contents (polybench.h ~L208-214). Comparing two POLYBENCH_TIME
    binaries' stdout as "the computed result" silently compares timing
    noise instead -- this is exactly what earlier produced the retracted
    "2mm computes wrong results" / "durbin is numerically unstable"
    findings elsewhere in this project. Use compile_baseline_dump() below
    for a binary whose output is actually safe to correctness-check.
    """
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


def compile_baseline_dump(kernel_c: str, utils: str, source_dir: str, out_bin: str,
                          dataset: str = "LARGE_DATASET", timeout: int = 180):
    """Same source/flags as compile_baseline() but with -DPOLYBENCH_DUMP_ARRAYS
    instead of -DPOLYBENCH_TIME -- this binary's stdout is the real computed
    array contents and is safe to use as a correctness reference. Not timed
    (that's what the -DPOLYBENCH_TIME binary from compile_baseline() is for)."""
    polybench_c = str(Path(utils) / "polybench.c")
    defines = [f"-D{dataset}", "-DPOLYBENCH_DUMP_ARRAYS"]
    ok, err = compile_c(CLANG, [kernel_c, polybench_c], [utils, source_dir],
                         defines, out_bin, timeout=timeout)
    if not ok:
        return False, err
    return True, ""


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
                             work_dir: "str | None" = None, timeout: int = 180,
                             output_macro: str = "POLYBENCH_TIME"):
    """Compile kernel_c with a CUSTOM pass order (mem2reg always first),
    utils/polybench.c at plain -O3, link together. Returns (ok, err).

    output_macro: "POLYBENCH_TIME" (default, for timing -- stdout is just
    the elapsed time, NOT safe to correctness-check) or
    "POLYBENCH_DUMP_ARRAYS" (stdout is the real computed array contents,
    safe to correctness-check, not meaningfully timeable). Every call site
    that does BOTH must build twice, once with each macro, into different
    out_bin paths -- one binary's stdout is never valid for the other
    purpose. See compile_baseline()'s docstring for why this distinction
    exists at all (the retracted "2mm/durbin miscompile" findings)."""
    wd = Path(work_dir) if work_dir else Path(kernel_c).parent
    inc = [f"-I{utils}", f"-I{source_dir}"]
    defines = [f"-D{dataset}", f"-D{output_macro}"]

    kernel_raw_ll = wd / f"kernel_raw_{output_macro}.ll"
    kernel_opt_ll = wd / f"kernel_opt_{output_macro}.ll"
    kernel_opt_o = wd / f"kernel_opt_{output_macro}.o"
    utils_o = wd / f"polybench_o3_{output_macro}.o"
    polybench_c = str(Path(utils) / "polybench.c")

    # 1. Frontend-compile the kernel with all LLVM passes disabled -- gives
    #    canonical (alloca-based, non-SSA) IR, same shape as clang -O0 output.
    #
    #    -O3 (not -O1) is required even though every pass is disabled: the
    #    frontend optimization level still controls the function ATTRIBUTES it
    #    emits (inline hints, optsize, etc.), which the later `opt` pipeline
    #    depends on. Measured on gemm: -O1 frontend + default<O3> + llc -O3
    #    reached only 0.69x of the -O3 baseline, while -O3 frontend with the
    #    identical rest-of-pipeline reaches 1.01x (i.e. exactly reproduces the
    #    baseline, as it must). Using -O1 here silently capped every candidate
    #    at ~0.7x no matter how good its pass order was.
    ok, err = _run([CLANG, "-O3", "-Xclang", "-disable-llvm-passes"] + STD_FLAGS
                    + inc + defines + ["-S", "-emit-llvm", kernel_c,
                                        "-o", str(kernel_raw_ll)], timeout=timeout)
    if not ok:
        return False, f"frontend IR emit failed: {err}"

    # 2. Apply the candidate pass order (mem2reg first, unconditionally --
    #    without it every later pass sees the same alloca-heavy IR as -O0).
    # A stock LLVM pipeline like "default<O3>" is a MODULE pipeline and must
    # stand alone: prefixing it with the function pass mem2reg makes opt parse
    # the whole -passes string as a function pipeline and reject it with
    # "unknown function pass 'default<O3>'".
    if len(pass_order) == 1 and pass_order[0].startswith("default<"):
        passes_str = pass_order[0]
    else:
        passes_str = "mem2reg," + ",".join(pass_order) if pass_order else "mem2reg"
    ok, err = _run([OPT, f"-passes={passes_str}", "-S",
                    str(kernel_raw_ll), "-o", str(kernel_opt_ll)], timeout=timeout)
    if not ok:
        return False, f"opt -passes={passes_str} failed: {err}"

    # 3. Codegen the optimized IR to an object file with `llc -O3`.
    #
    #    MUST be llc, not `clang -c file.ll`: clang defaults to -O0 CODEGEN
    #    when no -O flag is given, which disables instruction scheduling,
    #    good register allocation, and the machine-level peepholes -- a
    #    ~3.4x slowdown that has nothing to do with the IR pass order being
    #    searched. (Passing `clang -O3 -c file.ll` is NOT the fix either: it
    #    would re-run the entire -O3 IR pipeline on top of the candidate
    #    order, erasing exactly the variable this harness exists to measure.
    #    llc runs backend codegen only.)
    #
    #    Measured on gemm (baseline -O3 = 0.577s): real -O3 IR codegen'd by
    #    `clang -c` -> 1.621s (0.36x), same IR by `llc -O3` -> 0.586s (0.98x).
    #    This single flag was the dominant cause of the systematic 0.2x-0.7x
    #    "regressions" previously reported for every pass-order candidate.
    ok, err = _run([LLC, "-O3", "-filetype=obj", str(kernel_opt_ll),
                    "-o", str(kernel_opt_o)], timeout=timeout)
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
