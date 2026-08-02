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

# ---------------------------------------------------------------------------
# Pipeline-string construction
# ---------------------------------------------------------------------------
# opt's -passes= parser infers ONE nesting level for a flat comma-separated
# list, from the first pass in it. So the moment a loop pass appears in a flat
# list, every following name is parsed as a loop pass too:
#
#   -passes=sroa,loop-rotate,instcombine
#     -> "unknown loop pass 'instcombine'"   (and the whole run fails)
#
# The AutoPass Reasoning Agent proposes orders like that constantly -- putting
# instcombine/simplifycfg/gvn after loop-rotate or licm is completely ordinary
# compiler practice. Joining its proposal with commas therefore threw away any
# candidate that interleaved loop and function passes, which is what consumed
# 19 of the first sweep's 147 evaluation rounds and cost four programs
# (susan_smoothing, tiff2bw, dijkstra, security_sha) all three of theirs.
#
# The fix is to emit what the flat list MEANS: group each run of consecutive
# loop passes into its own loop(...)/loop-mssa(...) adaptor inside the
# surrounding function(...) pipeline.
#
# Which level a pass belongs to is probed from opt itself rather than
# hardcoded, because the answer is neither guessable from the name nor stable
# across LLVM versions -- `loop-unroll` is a FUNCTION pass despite its name
# (it is a whole-function unroller), while `licm` is loop-level and
# additionally requires the MemorySSA-preserving adaptor.
_LEVEL_CACHE: dict = {}

# InstCombine ships a self-check that the pass reached a fixpoint within its
# iteration budget, and when it does not, LLVM does not warn -- it aborts:
#
#   LLVM ERROR: Instruction Combining on susan_edges did not reach a fixpoint
#   after 1 iterations. Use 'instcombine<no-verify-fixpoint>' ... to suppress
#
# The stock -O3 pipeline never trips this because instcombine only ever runs
# in positions its budget was tuned for. An agent proposing pass ORDERS puts
# it elsewhere constantly, and each abort killed the whole evaluation: this is
# the single cause of all 19 rounds the first PO sweep lost, and of four
# programs (susan_smoothing, tiff2bw, dijkstra, security_sha) losing all
# three of theirs and being recorded at 1.000x as if the search had honestly
# found nothing.
#
# Taking LLVM's own advice suppresses only the VERIFICATION, not any
# transformation -- where instcombine already converged the emitted IR is
# unchanged (verified byte-identical), and where it did not, we now get the
# partially-combined result the pipeline actually produces instead of an
# aborted run. Not reaching a fixpoint is a property of the pass order being
# measured, which is the thing under study; it is not a reason to discard the
# measurement.
_INSTCOMBINE_SPELLING = {
    "instcombine": "instcombine<no-verify-fixpoint>",
}


def _probe_pass_level(name: str, probe_ll: str, timeout: int = 60) -> str:
    """"function" | "loop" | "loop-mssa" | "module" | "unknown"."""
    if name in _LEVEL_CACHE:
        return _LEVEL_CACHE[name]
    inner = name
    if name.startswith("loop-mssa(") and name.endswith(")"):
        inner = name[len("loop-mssa("):-1]
    level = "unknown"
    # Probe INNERMOST first. opt silently wraps a loop pass in an adaptor when
    # it appears inside function(...), so `function(loop-rotate)` succeeds and
    # testing that first would label every loop pass "function" -- which is
    # exactly the mislabel that let `loop-rotate,instcombine` be emitted flat,
    # where opt infers a LOOP pipeline from the first pass and then rejects
    # instcombine as "unknown loop pass". A function pass inside loop(...) has
    # no such implicit adaptor, so the narrow levels are the discriminating
    # ones and must be tried before the permissive ones.
    # `loop` before `loop-mssa`: the MemorySSA adaptor also accepts plain loop
    # passes, so testing it first would push everything into loop-mssa and
    # silently add MemorySSA preservation the candidate never asked for. Only
    # passes that genuinely require it (licm: "LICM requires MemorySSA
    # (loop-mssa)") fail under plain loop(...) and fall through to it.
    for lvl, tmpl in (("loop", "function(loop({}))"),
                      ("loop-mssa", "function(loop-mssa({}))"),
                      ("function", "function({})"),
                      ("module", "{}")):
        ok, _ = _run([OPT, f"-passes={tmpl.format(inner)}",
                      "-S", probe_ll, "-o", "/dev/null"], timeout=timeout)
        if ok:
            level = lvl
            break
    _LEVEL_CACHE[name] = level
    return level


def build_pipeline_string(pass_order: list, probe_ll: str) -> str:
    """Turn a flat pass list into a correctly nested opt -passes= string.

    Consecutive loop passes are collected into one adaptor so the ordering the
    Reasoning Agent asked for is preserved exactly; a function pass appearing
    between two loop passes correctly splits them into two adaptors, because
    that is what the requested order means.
    """
    # A stock LLVM pipeline like "default<O3>" is a MODULE pipeline and must
    # stand alone: prefixing it with the function pass mem2reg makes opt parse
    # the whole -passes string as a function pipeline and reject it with
    # "unknown function pass 'default<O3>'".
    if len(pass_order) == 1 and pass_order[0].startswith("default<"):
        return pass_order[0]

    # mem2reg first, unconditionally -- without it every later pass sees the
    # same alloca-heavy IR as -O0. Don't double it up when the caller (or the
    # Reasoning Agent) already asked for it first.
    seq = list(pass_order or [])
    if not seq or seq[0] != "mem2reg":
        seq = ["mem2reg"] + seq

    parts: list = []          # entries inside the outer function(...)
    module_tail: list = []    # module passes can't live inside function(...)

    for p in seq:
        p = _INSTCOMBINE_SPELLING.get(p, p)
        level = _probe_pass_level(p, probe_ll)
        inner = p[len("loop-mssa("):-1] if (p.startswith("loop-mssa(") and p.endswith(")")) else p
        if level in ("loop", "loop-mssa"):
            # ONE adaptor per loop pass, never a shared loop(a,b,c). Grouping
            # would change what the requested order means: a shared adaptor
            # runs a,b,c over each loop in turn, while separate adaptors run a
            # over every loop, then b over every loop. opt's own implicit
            # adaptation of a flat list does the latter, and the previous sweep
            # was measured that way, so grouping here would silently make the
            # PO numbers incomparable with it. Verified byte-identical IR
            # against the old flat form on mixed loop/function pipelines.
            parts.append(f"{level}({inner})")
        elif level == "module":
            module_tail.append(p)
        else:                       # function, or unknown -> let opt decide
            parts.append(p)

    pipeline = f"function({','.join(parts)})" if parts else ""
    if module_tail:
        pipeline = ",".join(([pipeline] if pipeline else []) + module_tail)
    return pipeline or "function(mem2reg)"


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
                             output_macro: str = "POLYBENCH_TIME",
                             opt_params: "list | None" = None):
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

    # 2. Apply the candidate pass order, nested into the adaptors opt expects
    #    (see build_pipeline_string -- it also prepends mem2reg).
    passes_str = build_pipeline_string(pass_order, str(kernel_raw_ll))
    # opt_params are plain `-flag=value` tuning knobs (e.g. -unroll-threshold=600)
    # passed alongside -passes=. AutoPass's Reasoning Agent tunes these as well
    # as the order -- see TUNABLE_PARAMS in pass_list_autopass.py.
    param_flags = list(opt_params or [])
    ok, err = _run([OPT, f"-passes={passes_str}"] + param_flags
                   + ["-S", str(kernel_raw_ll), "-o", str(kernel_opt_ll)], timeout=timeout)
    if not ok:
        return False, f"opt -passes={passes_str} {' '.join(param_flags)} failed: {err}"

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
