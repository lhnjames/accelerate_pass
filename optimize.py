#!/usr/bin/env python3
"""
COMET unified optimizer — generic, evidence-driven, multi-round.

Each round a single LLM call receives:
  ① LLVM pass pipeline graph  (which passes ran/missed, in execution order)
  ② IR statistics              (vector_ops, fmul, phi_nodes, …)
  ③ Compiler remarks           (pass / missed / analysis, filtered to kernel)
  ④ Kernel source              (current best version)
  ⑤ Hardware profile           (CPU model, SIMD width, cache sizes)
  ⑥ Optimization history       (every prior round: strategy, speedup, error)
  ⑦ Strategy memory            (what worked, what regressed, what to avoid)

The LLM outputs a multi-strategy JSON covering three channels:
  Ch1  -mllvm threshold flags  — LLVM pass cost-model knobs
  Ch2  #pragma clang loop      — per-loop !llvm.loop metadata hints
  Ch3  Source rewrite          — loop tiling, register blocking, interchange, …

Channels are evaluated independently then jointly combined.

Usage:
  python optimize.py --program path/to/kernel.c --rounds 5
  python optimize.py --program path/to/kernel.c --graph-only   # pass graph only
"""
import os, sys, re, argparse, subprocess, tempfile, statistics, itertools, json, dataclasses, shutil, time
from pathlib import Path
from typing import List, Dict, Any, Optional, Tuple

sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from src.config import ConfigLoader
from src.llm_client import LLMClient, strip_json_fences
from src.compiler_manager import CompilerRunner
from src.pass_graph import generate_pass_graph
from src.datasets import detect_dataset_type
from src.run_logger import RunLogger, LoggingLLMClient

from tune_param import (
    find_polybench_utilities,
    get_cpu_info,
    get_kernel_line_range,
    extract_remarks_by_pass,
    filter_remarks_to_kernel,
    extract_o3_passes_for_kernel,
    extract_kernel_ir,
    discover_options_from_help,
    get_ir_stats,
    score_passes,
    run_timing as tp_run_timing,
    compile_binary,
    PASS_CATEGORIES,
    PRAGMA_LOOP_HINTS,
    _pick_probe_value,
    extract_rich_remarks_yaml,
    format_rich_remark_for_prompt,
)

from tune_source import (
    extract_kernel_function,
    extract_header_macros,
    extract_numbers_from_dump,
    extract_vectorization_remarks,
    get_cpu_cache_info as ts_get_cache_info,
    compare_outputs,
    run_timing as ts_run_timing,
    compile_c,
    _strip_fences,
    _build_prompt,
    analyze_kernel_patterns,
    analyze_precision_failure,
    _build_precision_fix_prompt,
    _build_compile_fix_prompt,
    _detect_triangular_loop,
    _detect_inplace_stencil,
    _detect_inplace_factorization,
    _detect_strided_inner_loop,
)

from src.remarks import (
    extract_rich_remarks_yaml as _extract_rich_remarks_src,
    format_rich_remarks_for_source_prompt,
)
from src.diagnostics import clean_clang_diagnostics
from src.correctness import detect_correctness_mode, check_correctness
from src.hotspot import select_hotspot_target, select_hotspot_targets


# ── Single-shot timing (for interleaved confirmation runs) ───────────────────

def _single_shot_ms(bin_path: str, pin_cpu: "int | None" = None) -> float:
    """One execution, no internal warmup/median — caller controls repetition
    and interleaving. Returns -1.0 on failure."""
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [str(bin_path)]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if res.returncode != 0:
            return -1.0
        out = res.stdout.strip() or res.stderr.strip()
        return float(out.split()[-1]) * 1000.0
    except Exception:
        return -1.0


def confirm_result(base_bin: str, best_bin: str, runs: int,
                    pin_cpu: "int | None" = None) -> dict:
    """Re-measure baseline and best candidate interleaved (base, best, base,
    best, ...) to cancel slow system-load drift that a single measurement
    taken at the start of a multi-minute agent run cannot control for.

    Reports the confirmed speedup as the median of per-pair ratios (paired
    measurement), plus IQR as a cheap non-parametric uncertainty estimate —
    a single noisy step-level measurement is not sufficient evidence for a
    paper-reportable number.
    """
    # one throwaway warmup per binary (cache/branch predictor/turbo ramp-up)
    _single_shot_ms(base_bin, pin_cpu)
    _single_shot_ms(best_bin, pin_cpu)

    ratios, base_ms, best_ms = [], [], []
    for _ in range(max(1, runs)):
        b = _single_shot_ms(base_bin, pin_cpu)
        o = _single_shot_ms(best_bin, pin_cpu)
        if b > 0 and o > 0:
            base_ms.append(b)
            best_ms.append(o)
            ratios.append(b / o)

    if not ratios:
        return {"ok": False}

    ratios_sorted = sorted(ratios)
    n = len(ratios_sorted)
    q1 = ratios_sorted[n // 4]
    q3 = ratios_sorted[(3 * n) // 4] if n > 1 else ratios_sorted[0]
    return {
        "ok": True,
        "n": n,
        "confirmed_speedup": statistics.median(ratios),
        "speedup_iqr": [q1, q3],
        "base_median_ms": statistics.median(base_ms),
        "best_median_ms": statistics.median(best_ms),
        "base_stdev_pct": (statistics.stdev(base_ms) / statistics.mean(base_ms) * 100.0) if n > 1 else 0.0,
        "best_stdev_pct": (statistics.stdev(best_ms) / statistics.mean(best_ms) * 100.0) if n > 1 else 0.0,
    }


def _single_shot_ms_external(bin_path: str, pin_cpu: "int | None" = None) -> float:
    """One execution, EXTERNAL wall-clock timing (time.monotonic() wrapped
    around the subprocess) -- this is deliberately a different methodology
    from _single_shot_ms(), which parses the binary's own stdout-reported
    time. tp_run_timing()/ts_run_timing() (src/build_utils.run_timing,
    used everywhere else in this file for baseline_time and Phase A/B flag
    screening) are ALSO external-clock. Mixing the two -- taking a ratio or
    absolute ms from stdout-self-report and dividing it into (or comparing
    it against) an externally-measured baseline_time -- silently produces
    nonsense: observed live, SPEC lbm_r's confirm step reported "134.346x"
    because its self-reported time only covers a fraction of true wall time
    for that binary, while baseline_time came from run_timing()'s external
    clock. Use this (and confirm_result_external below), not confirm_result,
    anywhere the result needs to compare against a run_timing()-sourced
    baseline_time."""
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [str(bin_path)]
    t0 = time.monotonic()
    try:
        res = subprocess.run(cmd, capture_output=True, timeout=120)
    except Exception:
        return -1.0
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    return elapsed_ms if res.returncode == 0 else -1.0


def confirm_result_external(base_bin: str, best_bin: str, runs: int,
                            pin_cpu: "int | None" = None) -> dict:
    """Same alternating-measurement / paired-median technique as
    confirm_result(), but using _single_shot_ms_external() throughout --
    see its docstring for why this matters. Same return shape."""
    _single_shot_ms_external(base_bin, pin_cpu)
    _single_shot_ms_external(best_bin, pin_cpu)

    ratios, base_ms, best_ms = [], [], []
    for _ in range(max(1, runs)):
        b = _single_shot_ms_external(base_bin, pin_cpu)
        o = _single_shot_ms_external(best_bin, pin_cpu)
        if b > 0 and o > 0:
            base_ms.append(b)
            best_ms.append(o)
            ratios.append(b / o)

    if not ratios:
        return {"ok": False}

    ratios_sorted = sorted(ratios)
    n = len(ratios_sorted)
    q1 = ratios_sorted[n // 4]
    q3 = ratios_sorted[(3 * n) // 4] if n > 1 else ratios_sorted[0]
    return {
        "ok": True,
        "n": n,
        "confirmed_speedup": statistics.median(ratios),
        "speedup_iqr": [q1, q3],
        "base_median_ms": statistics.median(base_ms),
        "best_median_ms": statistics.median(best_ms),
        "base_stdev_pct": (statistics.stdev(base_ms) / statistics.mean(base_ms) * 100.0) if n > 1 else 0.0,
        "best_stdev_pct": (statistics.stdev(best_ms) / statistics.mean(best_ms) * 100.0) if n > 1 else 0.0,
    }


# ── Optimization history & agent memory ──────────────────────────────────────

@dataclasses.dataclass
class StepRecord:
    """One agent step: one action chosen by the LLM."""
    step_num:   int
    action:     str    # try_flags | try_pragma | rewrite_source | done
    reasoning:  str
    speedup:    float = 1.0
    strategy:   str   = ""   # human-readable description of what was tried
    error:      str   = ""
    flags:      list  = dataclasses.field(default_factory=list)
    has_source: bool  = False
    perf_stats: dict  = dataclasses.field(default_factory=dict)  # perf 硬件计数器
    snapshot_path: str = ""  # 快照文件路径
    improvement_analysis: str = ""  # 规划 LLM 对"为何改进不够大/为何未改进"的分析

    def summary_line(self) -> str:
        tag = f"步骤{self.step_num}[{self.action}]"
        if self.error:
            return f"{tag}: 失败 -- {self.error[:80]}"
        perf = ""
        if self.perf_stats:
            ipc = self.perf_stats.get("ipc")
            llc = self.perf_stats.get("llc_miss_rate")
            if ipc is not None:
                perf = f" IPC={ipc:.2f}"
            if llc is not None:
                perf += f" LLC_miss={llc:.1f}%"
        return f"{tag}: {self.speedup:.3f}x  [{self.strategy or self.action}]{perf}"

    def detail_lines(self) -> List[str]:
        lines = [self.summary_line()]
        if self.reasoning:
            lines.append(f"  推理: {self.reasoning[:200]}")
        if self.improvement_analysis:
            lines.append(f"  改进分析: {self.improvement_analysis}")
        if self.flags:
            lines.append(f"  Flags: {' '.join(str(f) for f in self.flags[:8])}")
        if self.perf_stats and not self.perf_stats.get("error"):
            hints = self.perf_stats.get("bottleneck_hints", [])
            cpu   = self.perf_stats.get("cpu_util_pct")
            if hints:
                lines.append(f"  perf瓶颈: {', '.join(hints)}"
                             + (f"  CPU利用率={cpu:.1f}%" if cpu else ""))
        if self.snapshot_path:
            lines.append(f"  快照: {self.snapshot_path}")
        return lines


class OptimizationHistory:
    """
    Persistent step-level memory for the agent optimizer.

    Every action the LLM takes is recorded here. On each new step the LLM
    receives the full history so it can:
      - Avoid repeating strategies that regressed or errored
      - Build on approaches that worked
      - Recognise convergence and call "done"
    """

    def __init__(self):
        self.steps:         List[StepRecord]    = []
        self.best_speedup:  float               = 1.0
        self.best_combo:    str                 = "baseline"
        self.strategy_speedups: Dict[str, float] = {}
        self.failed_strategies: List[str]       = []
        self.errored_strategies: List[str]      = []
        # ── 参数轨迹：每次有新 flags 的步骤都记录 ─────────────────────────────
        self.flags_timeline: List[dict]         = []
        # 当前最优 flags 组（与 best_speedup 对应，可能来自不同步骤）
        self.best_flags_ever: list              = []
        self.best_flags_speedup: float          = 1.0
        # 连续 pragma 失败计数（每次 try_pragma 成功或换其他 action 后归零）
        self.consecutive_pragma_errors: int     = 0

    # ── update ────────────────────────────────────────────────────────────────

    def record(self, step_num: int, result: dict) -> None:
        """将一个 agent 步骤的结果记录到 history。"""
        action        = result.get("action",        "unknown")
        reasoning     = result.get("reasoning",     "")
        speedup       = result.get("speedup",       1.0)
        strategy      = result.get("strategy",      "")
        error         = result.get("error",         "")
        flags         = result.get("flags",         [])
        has_src       = result.get("source") is not None
        perf_stats    = result.get("perf_stats",    {})
        snapshot_path = result.get("snapshot_path", "")
        improvement_analysis = result.get("improvement_analysis", "")

        rec = StepRecord(
            step_num=step_num, action=action, reasoning=reasoning,
            speedup=speedup, strategy=strategy, error=error,
            flags=flags, has_source=has_src,
            perf_stats=perf_stats, snapshot_path=snapshot_path,
            improvement_analysis=improvement_analysis,
        )
        self.steps.append(rec)

        key = f"{action}:{strategy}" if strategy else action
        if error:
            if key not in self.errored_strategies:
                self.errored_strategies.append(f"{key} — {error[:60]}")
            if action == "try_pragma":
                self.consecutive_pragma_errors += 1
        else:
            if action == "try_pragma":
                self.consecutive_pragma_errors = 0
            self.strategy_speedups[key] = max(
                self.strategy_speedups.get(key, 1.0), speedup)
            if speedup < 0.99:
                if key not in self.failed_strategies:
                    self.failed_strategies.append(f"{key} ({speedup:.3f}x)")

        # 记录参数轨迹
        if flags:
            entry = {
                "step": step_num, "action": action,
                "flags": flags[:],
                "flags_str": " ".join(flags),
                "speedup": speedup,
                "with_source": has_src,
            }
            self.flags_timeline.append(entry)
            if speedup > self.best_flags_speedup:
                self.best_flags_speedup = speedup
                self.best_flags_ever    = flags[:]

        if speedup > self.best_speedup:
            self.best_speedup = speedup
            self.best_combo   = strategy or action

    # ── read ──────────────────────────────────────────────────────────────────

    def to_prompt_section(self, window: int = 8) -> str:
        """Render history as a text block for the LLM agent prompt."""
        if not self.steps:
            return ""

        lines = [
            "═" * 64,
            f"## Optimization History  ({len(self.steps)} steps completed)",
            f"Overall best so far: {self.best_speedup:.3f}x  [{self.best_combo}]",
            "",
        ]

        older = self.steps[:-window] if len(self.steps) > window else []
        if older:
            lines.append("Earlier steps (summary):")
            for s in older:
                lines.append("  " + s.summary_line())
            lines.append("")

        recent = self.steps[-window:]
        lines.append(f"Recent {len(recent)} steps (full detail):")
        for s in recent:
            for l in s.detail_lines():
                lines.append("  " + l)
            lines.append("")

        good = sorted(
            [(k, v) for k, v in self.strategy_speedups.items() if v > 1.005],
            key=lambda x: -x[1]
        )
        if good:
            lines.append("Strategies that HELPED (>0.5% gain):")
            for k, v in good[:10]:
                lines.append(f"  ✓ {k}: {v:.3f}x")
            lines.append("")

        if self.failed_strategies:
            lines.append("Strategies that REGRESSED — do NOT repeat:")
            for s in self.failed_strategies[-8:]:
                lines.append(f"  ✗ {s}")
            lines.append("")

        if self.errored_strategies:
            lines.append("Strategies that ERRORED (compile / precision failures):")
            for s in self.errored_strategies[-8:]:
                lines.append(f"  ✗ {s}")
            lines.append("")

        # perf 指标变化趋势
        perf_steps = [s for s in self.steps if s.perf_stats and not s.perf_stats.get("error")]
        if len(perf_steps) >= 2:
            lines.append("perf 指标变化趋势（步骤 → IPC / LLC miss）:")
            for s in perf_steps[-5:]:
                ipc = s.perf_stats.get("ipc", "?")
                llc = s.perf_stats.get("llc_miss_rate", "?")
                cpu = s.perf_stats.get("cpu_util_pct", "?")
                hints = ", ".join(s.perf_stats.get("bottleneck_hints", []))
                lines.append(
                    f"  步骤{s.step_num} {s.speedup:.3f}x: "
                    f"IPC={ipc}  LLC_miss={llc}%  CPU={cpu}%  [{hints}]"
                )
            lines.append("")

        if len(self.steps) >= 4:
            recent_sps = [s.speedup for s in self.steps[-4:] if not s.error]
            if recent_sps and max(recent_sps) - min(recent_sps) < 0.005:
                lines.append(
                    "⚠ 收敛警告: 最近4步加速比变化<0.5%，建议换完全不同的策略或调用action=done。"
                )
                lines.append("")

        if self.consecutive_pragma_errors >= 2:
            lines.append(
                f"⛔ try_pragma 已连续失败 {self.consecutive_pragma_errors} 次（编译错误）。"
                "请勿再使用 try_pragma——改用 rewrite_source 或 try_flags。"
            )
            lines.append("")

        # 参数轨迹（让 LLM 知道哪些 flags 被测过，哪些有效）
        if self.flags_timeline:
            lines.append("参数演化轨迹（所有曾经有效的 flags 组合）:")
            for e in self.flags_timeline:
                src_tag = " [+source]" if e["with_source"] else ""
                lines.append(f"  步骤{e['step']} [{e['action']}]{src_tag}: "
                              f"{e['speedup']:.3f}x  {e['flags_str']}")
            if self.best_flags_ever:
                lines.append(f"当前最优参数组: {' '.join(self.best_flags_ever)} "
                             f"({self.best_flags_speedup:.3f}x)")
                lines.append("  ⟹ try_flags 时用 base=\"current_best\" 可在当前最优源码上重新搜参数。")
            lines.append("")

        return "\n".join(lines)

    def to_dict(self) -> dict:
        return {
            "best_speedup":       self.best_speedup,
            "best_combo":         self.best_combo,
            "steps":              [dataclasses.asdict(s) for s in self.steps],
            "strategy_speedups":  self.strategy_speedups,
            "failed_strategies":  self.failed_strategies,
            "errored_strategies": self.errored_strategies,
            "flags_timeline":     self.flags_timeline,
            "best_flags_ever":    self.best_flags_ever,
            "best_flags_speedup": self.best_flags_speedup,
        }


def _diagnose_precision_error(msg: str) -> str:
    """Return a targeted CAUSE/FIX message based on error statistics in msg."""
    import re
    m_count = re.search(r'(\d+)/(\d+)\s+elements', msg)
    m_abs   = re.search(r'Max abs_diff=([\d.eE+-]+\d)', msg)
    n_errors = int(m_count.group(1)) if m_count else 0
    n_total  = int(m_count.group(2)) if m_count else 1
    max_abs  = float(m_abs.group(1))  if m_abs   else 0.0

    error_frac = n_errors / max(n_total, 1)

    if error_frac > 0.02 or max_abs > 0.05:
        # Many elements wrong or large diff → iteration-order / dependency violation
        return (
            "CAUSE: Loop-carried dependency violated. This kernel performs IN-PLACE updates "
            "where the result at element [i][j] depends on values already written earlier "
            "in the SAME sweep (Gauss-Seidel, LU, Cholesky, etc.).\n"
            "FIX:\n"
            "  - DO NOT copy the array to a temporary before updating (Jacobi-style snapshot "
            "changes the algorithm and gives wrong results).\n"
            "  - DO NOT reorder or tile the outer loop (t or k) in ways that change which "
            "elements have already been updated when [i][j] is computed.\n"
            "  - DO NOT add `ivdep`, `#pragma clang loop vectorize(enable)`, or any pragma "
            "that tells the compiler to ignore loop-carried dependencies.\n"
            "  - SAFE: scalar replacement of a single row/column at a time, row-pointer "
            "tricks, loop-invariant hoisting WITHIN one i-iteration."
        )
    else:
        # Few elements, tiny diff → FP reassociation / multi-accumulator
        return (
            "CAUSE: Floating-point reordering. Changing the order of additions "
            "(e.g. multi-accumulator split, SIMD reduction, or loop interchange on the "
            "summation dimension) produces slightly different rounding.\n"
            "FIX:\n"
            "  - Use exactly ONE scalar `double` accumulator per output element.\n"
            "  - NEVER split: `acc0 += A[k]; acc1 += B[k]; result = acc0 + acc1;` — "
            "sum in a single `acc` instead.\n"
            "  - Do NOT add `#pragma clang loop vectorize(enable)` to the reduction loop "
            "(it enables SIMD horizontal reduction which reorders additions).\n"
            "  - The innermost summation must iterate in the SAME sequential order as "
            "the original code."
        )


# ── Evidence collection (shared between unified and legacy rounds) ────────────

def _make_utils_scratch_copy(utils: Path, output_dir: Path) -> Path:
    """Copy a shim's utilities/ dir into this run's own output dir, so
    rewrite_source can persist a change to a hot function living in
    utils/polybench.c (see src/hotspot.py) across the agent's later steps
    by simply overwriting a file at a stable path -- without ever touching
    the shared, git-tracked original."""
    scratch = output_dir / "utils_scratch"
    if scratch.exists():
        shutil.rmtree(scratch)
    shutil.copytree(utils, scratch)
    return scratch


def collect_all_evidence(src: str, config, runner: CompilerRunner,
                         kernel_name: str,
                         output_dir: "str | None" = None) -> dict:
    """
    Run one-time expensive analysis: IR extraction, pass pipeline, remarks,
    IR stats.  Returns a dict with all evidence fields consumed by the LLM.
    """
    clang = config.compiler.clang_path
    opt   = config.compiler.opt_path
    utils = find_polybench_utilities(src)
    source_dir = Path(src).resolve().parent

    # ── utils 私有可写副本（支持热点函数在 utils/polybench.c 里的改写持久化）──
    # `utils` 原本直接指向共享的、git 跟踪的 shim 目录（如
    # SPEC_shim_root/mcf_r/SPEC_shim/utilities/）。rewrite_source 现在能改写
    # 热点函数落在 utils/polybench.c 里的情况（见 src/hotspot.py），改动要
    # 想跨步骤持久化，同时绝不能碰共享的原始文件——把它整个复制到本次 run
    # 私有的 scratch 目录，后续所有步骤统一用这个路径，"持久化"就只是往这
    # 一个稳定路径写文件，不需要在主循环里新增字段专门传递 utils 状态。
    if utils and output_dir:
        utils = _make_utils_scratch_copy(utils, Path(output_dir))

    # ── 静态分析兜底路径（无 PolyBench utilities）────────────────────────────
    if not utils:
        print("  PolyBench utilities 未找到 — 使用静态分析模式（无运行时数据）")
        try:
            from src.static_optimizer import (
                analyze_source_patterns, analyze_ir_patterns,
                infer_bottleneck_without_data, format_static_analysis_summary,
            )
            with open(src) as _f:
                full_src_fb = _f.read()
            kernel_text_fb, _, _ = extract_kernel_function(full_src_fb, kernel_name)

            # 尝试 IR 提取（不需要 polybench utilities）
            ok_fb, o0_ir_fb, _ = runner.extract_ir(src)
            ir_text_fb = ""
            kpasses_fb = []
            pg_fb = {}
            if ok_fb:
                kpasses_fb = extract_o3_passes_for_kernel(opt, o0_ir_fb, kernel_name)
                ir_text_fb = extract_kernel_ir(o0_ir_fb, kernel_name, max_lines=150)
                out_dir_fb = output_dir or "outputs"
                Path(out_dir_fb).mkdir(exist_ok=True, parents=True)
                pg_fb = generate_pass_graph(opt, o0_ir_fb, kernel_name,
                                            output_dir=out_dir_fb)
                try:
                    os.unlink(o0_ir_fb)
                except Exception:
                    pass

            patterns_fb   = analyze_source_patterns(full_src_fb)
            ir_pat_fb     = analyze_ir_patterns(ir_text_fb) if ir_text_fb else {}
            bottlenecks_fb = infer_bottleneck_without_data(patterns_fb, ir_pat_fb)
            static_summary = format_static_analysis_summary(
                patterns_fb, bottlenecks_fb, ir_pat_fb)
            print(f"  静态推断瓶颈: {bottlenecks_fb[0][0] if bottlenecks_fb else 'unknown'}")

            return {
                "kernel_name":       kernel_name,
                "kernel_text":       kernel_text_fb or "",
                "kernel_ir":         ir_text_fb,
                "kline_start":       1,
                "kline_end":         9999,
                "kernel_remarks":    {},
                "kernel_passes":     kpasses_fb,
                "baseline_stats":    {},
                "ir_diff_info":      [],
                "ir_pass_diffs":     {},
                "top_passes":        [],
                "discovered_opts":   {},
                "pass_graph":        pg_fb,
                "utils":             None,
                "source_dir":        source_dir,
                "baseline_perf":     {"bottleneck_hints":
                                      [b[0] for b in bottlenecks_fb]},
                "targeted_passes":   [],
                "missed_counts":     {},
                "correctness_mode":  "exit_only",
                "static_summary":    static_summary,
                "is_static_fallback": True,
            }
        except Exception as _fb_e:
            print(f"  静态分析兜底失败: {_fb_e}")
            return {}

    # Kernel line range
    kline_start, kline_end = get_kernel_line_range(src, kernel_name)

    # Compiler remarks (text-based, for compatibility)
    all_remarks    = extract_remarks_by_pass(clang, src, utils, source_dir)
    kernel_remarks = filter_remarks_to_kernel(all_remarks, kline_start, kline_end)

    # O0 IR → pass pipeline
    ok, o0_ir, err = runner.extract_ir(src)
    if not ok:
        print(f"  IR extraction failed: {err}")
        return {}
    kernel_passes  = extract_o3_passes_for_kernel(opt, o0_ir, kernel_name)
    kernel_ir_text = extract_kernel_ir(o0_ir, kernel_name, max_lines=150)

    # ── 富 YAML remarks（精确到代码行/列/向量化因子/源码片段）─────────────────
    rich_remarks: dict = {}
    try:
        print("  提取富 YAML remarks（含代码位置、向量化因子等）...")
        rich_remarks = extract_rich_remarks_yaml(clang, src, utils, source_dir, kernel_name)
        n_miss_rich = sum(
            sum(1 for e in ents if e.get("type") == "missed")
            for ents in rich_remarks.values()
        )
        print(f"  富 remarks: {len(rich_remarks)} passes, {n_miss_rich} missed")
    except Exception as _re:
        print(f"  富 remarks 提取失败（非致命）: {_re}")

    # ── Per-pass IR diff（每个 pass 是否真正修改了 IR）──────────────────────
    ir_pass_diffs: dict = {}
    try:
        from src.ir_diff import check_passes_ir_changes
        print("  检查每个 pass 的 IR 修改情况（前20个）...")
        ir_pass_diffs = check_passes_ir_changes(
            opt, o0_ir, kernel_passes[:20], kernel_name=kernel_name)
        n_fired = sum(1 for v in ir_pass_diffs.values()
                      if v.get("changed") and not v.get("skipped"))
        n_noop  = sum(1 for v in ir_pass_diffs.values()
                      if not v.get("changed") and not v.get("skipped"))
        print(f"  IR diff: {n_fired} passes FIRED, {n_noop} no-op")
    except Exception as _e:
        print(f"  IR diff 分析失败（非致命）: {_e}")

    # Pass graph visualization + summary
    out_dir   = output_dir or "outputs"
    Path(out_dir).mkdir(exist_ok=True, parents=True)
    pg = generate_pass_graph(opt, o0_ir, kernel_name,
                             output_dir=out_dir,
                             remarks_by_pass=kernel_remarks,
                             ir_diffs=ir_pass_diffs)
    if pg:
        print(f"  Pass graph → {pg.get('dot_path', '?')}")
        if Path(pg.get("png_path", "")).exists():
            print(f"  Pass graph PNG → {pg['png_path']}")

    # IR baseline stats
    baseline_stats = get_ir_stats(clang, src, utils, source_dir, kernel_name)

    # Discover tunable options for the passes that actually ran on this kernel
    discovered_opts = discover_options_from_help(opt, kernel_passes)
    scored = score_passes(kernel_passes, kernel_remarks, discovered_opts)
    top_passes = scored[:8]

    ir_diff_info = []
    for _, pname, _, _, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        for opt_entry in opts[:2]:
            flag = opt_entry["flag"].lstrip("-")
            probe = _pick_probe_value(flag)
            if probe is None:
                continue
            stats_mod = get_ir_stats(clang, src, utils, source_dir, kernel_name,
                                     extra_mllvm=[f"-{flag}={probe}"])
            if baseline_stats and stats_mod:
                dv = stats_mod.get("vector_ops", 0) - baseline_stats.get("vector_ops", 0)
                di = stats_mod.get("total_instr", 0) - baseline_stats.get("total_instr", 0)
                if dv != 0 or di != 0:
                    ir_diff_info.append(
                        f"  -{flag}={probe}: Δvec={dv:+d}, Δinstr={di:+d}"
                    )

    try:
        os.unlink(o0_ir)
    except Exception:
        pass

    # Kernel source text
    with open(src) as f:
        full_src = f.read()
    kernel_text, _, _ = extract_kernel_function(full_src, kernel_name)

    # ── 严格筛选：只保留有 missed remarks 且有可调参数的 O3 pass ──────────────
    # 这样 LLM 只会看到真正有优化空间的 pass，避免"调了也没用"的噪声
    missed_counts: Dict[str, int] = {}
    for pname, entries in kernel_remarks.items():
        mc = sum(1 for e in entries if e.get("type") == "missed")
        if mc > 0:
            missed_counts[pname] = mc

    # top_passes 重新筛选：有 missed remarks 优先，有可调参数次之
    filtered_top = []
    for item in scored:
        _, pname, cat, mc, _ = item
        has_opts = bool(discovered_opts.get(pname, []))
        has_missed = pname in missed_counts
        if has_missed and has_opts:
            filtered_top.append(item)
    # 如果严格过滤后太少，补上有 missed 但无参数的（可能需要源码重写提示）
    if len(filtered_top) < 3:
        for item in scored:
            _, pname, cat, mc, _ = item
            if item not in filtered_top and pname in missed_counts:
                filtered_top.append(item)
    top_passes = filtered_top[:8] if filtered_top else scored[:4]

    # ── 基线 profile（perf + 可选 VTune）────────────────────────────────────
    baseline_perf: dict = {}
    print("  收集基线硬件计数器（perf stat"
          + (" + VTune" if config.profiling.vtune_enabled else "")
          + "）...")
    try:
        polybench_c_path = utils / "polybench.c"
        import tempfile as _tf
        _bbin = _tf.NamedTemporaryFile(delete=False, suffix="_perf_base")
        _bbin.close()
        _ok, _ = compile_binary(clang, src, polybench_c_path, utils, source_dir,
                                Path(_bbin.name))
        if _ok:
            from src.perf_analysis import collect_combined_profile, format_perf_summary
            vtune_dir = str(Path(output_dir or "outputs") / f"{kernel_name}_vtune")
            baseline_perf = collect_combined_profile(
                _bbin.name,
                vtune_enabled=config.profiling.vtune_enabled,
                vtune_result_dir=vtune_dir,
                runs=2,
                pin_cpu=None,
            )
            print(format_perf_summary(baseline_perf))
        try:
            os.unlink(_bbin.name)
        except Exception:
            pass
    except Exception as _e:
        print(f"  profile 收集失败（非致命）: {_e}")

    # ── 逆向 pass 推断：瓶颈 → 最应调整的 O3 pass ─────────────────────────
    targeted_passes: list = []
    try:
        from src.perf_analysis import get_targeted_passes
        bottleneck_hints = baseline_perf.get("bottleneck_hints", ["unknown"])
        targeted_passes = get_targeted_passes(
            bottleneck_hints, kernel_passes, kernel_remarks, discovered_opts)
        if targeted_passes:
            print(f"  逆向推断: 瓶颈={bottleneck_hints} → 优先pass: "
                  + ", ".join(t["pass_name"] for t in targeted_passes[:3]))
    except Exception as _e:
        print(f"  逆向推断失败（非致命）: {_e}")

    # 一次性检测正确性验证模式（polybench_dump / stdout_compare / exit_only）
    print("  检测正确性验证模式...")
    with tempfile.TemporaryDirectory() as _det_tmp:
        _det_tmp_path = Path(_det_tmp)
        cmode = _detect_polybench_mode(clang, src, utils, source_dir, _det_tmp_path)
    print(f"  正确性验证模式: {cmode}")

    return {
        "kernel_name":        kernel_name,
        "kernel_text":        kernel_text or "",
        "kernel_ir":          kernel_ir_text,
        "kline_start":        kline_start,
        "kline_end":          kline_end,
        "kernel_remarks":     kernel_remarks,
        "kernel_passes":      kernel_passes,
        "baseline_stats":     baseline_stats,
        "ir_diff_info":       ir_diff_info,
        "ir_pass_diffs":      ir_pass_diffs,      # 每个 pass 的 IR 修改情况
        "rich_remarks":       rich_remarks,       # YAML 富 remarks（含代码位置+VF）
        "top_passes":         top_passes,
        "discovered_opts":    discovered_opts,
        "pass_graph":         pg,
        "utils":              utils,
        "source_dir":         source_dir,
        "baseline_perf":      baseline_perf,      # perf + VTune 基线
        "targeted_passes":    targeted_passes,    # 逆向推断结果
        "missed_counts":      missed_counts,      # 每个 pass 的 missed remarks 数量
        "correctness_mode":   cmode,              # 正确性验证模式（已检测）
        "is_static_fallback": False,
    }


# ── Unified LLM prompt builder ────────────────────────────────────────────────

def _build_remarks_and_targeted_passes(ev: dict) -> dict:
    """
    编译器 remarks 摘要 + 逆向推断可调 pass 表（瓶颈 → 精准 pass 参数）。
    纯只读计算：从 ev 中提取 missed/passed remarks 和 targeted-pass 展示文本。
    """
    missed_lines = []
    missed_sorted = sorted(
        [(pname, entries) for pname, entries in ev["kernel_remarks"].items()
         if any(e["type"] == "missed" for e in entries)],
        key=lambda x: -sum(1 for e in x[1] if e["type"] == "missed")
    )
    for pname, entries in missed_sorted[:5]:
        misses = [e for e in entries if e["type"] == "missed"]
        missed_lines.append(f"\n**{pname}** ({len(misses)} 次 missed)")
        for e in misses[:4]:
            missed_lines.append(f"  行{e['line']}: \"{e['msg'][:100]}\"")

    passed_lines = []
    for pname, entries in ev["kernel_remarks"].items():
        ps = [e for e in entries if e["type"] == "passed"]
        if ps:
            passed_lines.append(f"  {pname}: {len(ps)} 次成功")

    targeted = ev.get("targeted_passes", [])
    baseline_perf = ev.get("baseline_perf", {})

    try:
        from src.perf_analysis import format_perf_summary, format_targeted_passes
        perf_summary_str  = format_perf_summary(baseline_perf)
        targeted_pass_str = format_targeted_passes(targeted, ev["discovered_opts"])
    except Exception:
        perf_summary_str  = "  perf 数据不可用"
        targeted_pass_str = "  逆向推断不可用"

    # ── 兜底：若逆向推断为空，退回到 top_passes ──────────────────────────────
    if not targeted or targeted_pass_str.strip() == "未找到与当前瓶颈匹配的可调 O3 pass。":
        mllvm_lines = []
        for _, pname, cat, mc, _ in ev["top_passes"]:
            opts = ev["discovered_opts"].get(pname, [])
            if opts and mc > 0:
                mllvm_lines.append(f"\n**{pname}** [类型={cat}, missed={mc}次]")
                for o in opts[:4]:
                    mllvm_lines.append(f"  {o['flag']}=<{o['type']}>  -- {o['desc']}")
        targeted_pass_str = "\n".join(mllvm_lines) if mllvm_lines else "  无可调参数（需源码重写）"

    return {
        "missed_lines":      missed_lines,
        "passed_str":        "\n".join(passed_lines) if passed_lines else "  (无记录)",
        "perf_summary_str":  perf_summary_str,
        "targeted_pass_str": targeted_pass_str,
        "baseline_perf":     baseline_perf,
    }


def _build_step_guidance(step_num: int, max_steps: int, forced_action: "str | None",
                         best_sp: float, best_desc: str, bottleneck_hints: list,
                         history: "OptimizationHistory",
                         strided_hint: "tuple[bool, str]" = (False, "")) -> str:
    """构建步骤引导语：根据强制阶段/历史完成情况告诉 LLM 这一步该做什么。

    strided_hint: (found, message) from _detect_strided_inner_loop() on the
    current target's code -- see the plateau-escalation block below for why
    this needs to reach here and not just the rewrite prompts.
    """
    remaining = max_steps - step_num + 1

    # Check whether each mandatory phase has been done successfully (with speedup)
    _done_flags   = any(s.action == "try_flags"      and not s.error for s in history.steps)
    _done_rewrite = any(s.action == "rewrite_source" and not s.error and s.speedup >= 1.01
                        for s in history.steps)
    _tried_rewrite = any(s.action == "rewrite_source" for s in history.steps)
    _rewrite_failed = _tried_rewrite and not _done_rewrite

    if forced_action == "try_flags":
        guidance = (
            f"【第{step_num}步 强制：pass 参数优化】\n"
            f"⚠ 你必须选择 action=\"try_flags\"。不得选择其他 action。\n"
            f"瓶颈类型为 {bottleneck_hints}，根据逆向推断部分列出的可调参数，"
            "生成候选值列表（参数描述中含 default=N，请推断更激进方向的值）。"
        )
    elif forced_action == "rewrite_source":
        guidance = (
            f"【第{step_num}步 强制：源码重写优化】\n"
            f"⚠ 你必须选择 action=\"rewrite_source\"。不得选择其他 action。\n"
            f"当前 pass 参数最优已达 {best_sp:.3f}x。\n"
            "现在必须通过源码结构变换（loop tiling、循环交换、scalar accumulator 等）"
            "进一步提升性能。给出精确的 strategy，说明针对哪个循环、哪个数组、做什么变换。"
        )
    elif best_sp < 1.005:
        guidance = (
            f"已经过 {step_num-1} 步，最佳加速比仅 {best_sp:.3f}x。"
            "该 kernel 可能存在根本性瓶颈（内存依赖、串行递推）。"
            "请尝试完全不同的结构变换，或解释为什么无法优化并调用 action=done。"
        )
    else:
        guidance = (
            f"第{step_num}步：当前最优 {best_sp:.3f}x [{best_desc}]。"
            "在历史成功策略基础上深化，避免重复失败策略。"
        )
        # step3：前两步都已完成，引导做 source+flags 组合搜索
        if step_num == 3 and _done_flags and _done_rewrite:
            guidance += (
                "\n【第3步核心任务】前两步已分别完成 pass 调参和源码重写。"
                "现在用 base=\"current_best\" 做 try_flags，"
                "在重写后的源码上重新搜索最优参数组合——新 IR 结构可能让不同参数更有效。"
                "这是 source+flags 协同优化的关键步骤。"
            )
        # 提醒还未完成的阶段
        elif _rewrite_failed:
            # 已试过重写但失败——鼓励换不同策略重试，而不是放弃
            failed_strats = [s.strategy for s in history.steps
                             if s.action == "rewrite_source" and (s.error or s.speedup < 1.01)]
            guidance += (
                f"\n⚠ 已尝试的源码重写策略均未奏效（见历史）。"
                f"请勿重复相同策略，但**可以（且应该）换完全不同的重写方向再试**。"
                f"例如：若之前用了 tiling 但 tile 过大，应换更小的 tile（32×32）；"
                f"若 tiling 方向有问题，可换循环交换（loop interchange）或标量累加器（scalar reduction）等。"
                f"已失败策略：{failed_strats[-2:]}。"
            )
        elif not _done_rewrite:
            guidance += (
                "\n⚠ 注意：本轮尚未做过源码重写。单纯调整 pass 参数的天花板有限，"
                "强烈建议此步骤选择 rewrite_source 做结构变换，配合已找到的最优 flags 组合效果更佳。"
            )
        elif not _done_flags:
            guidance += (
                "\n⚠ 注意：本轮尚未做过 pass 参数调整。"
                "建议用 base=\"current_best\" 重新做 try_flags，在新源码上搜索最优参数。"
            )

    # ── 平台期升级：静态分析已经发现了跨步访存/循环交换机会，但源码重写
    # 一直没真的去做，收益已经连续几次趋于平缓——这正是 symm 这次跑到
    # 4.5x 就卡住的情况：第2步选了 tiling（4.587x），之后5轮都在这条路径上
    # 微调（4.584x~4.645x之间），从未回头试循环交换（历史同一 kernel 靠
    # 循环交换直接跳到 9.8x）。allowed 变换列表里"循环交换"和"tiling"权重
    # 相同，全靠 LLM 现场决定选哪个，选中 tiling 之后就没有机制推它回头
    # 重新考虑——这里补上：连续两次成功的 rewrite_source 收益都没有明显
    # 超过前一次时，且从没试过循环交换，就升级成明确要求。
    strided_found, strided_msg = strided_hint
    if strided_found and forced_action != "try_flags":
        _rewrite_ok = [s for s in history.steps
                      if s.action == "rewrite_source" and not s.error]
        _tried_interchange = any(
            s.strategy and re.search(
                r'循环交换|交换.{0,8}(循环|顺序)|loop\s*interchange|对调.{0,10}(循环|顺序)',
                s.strategy)
            for s in _rewrite_ok)
        if not _tried_interchange:
            if len(_rewrite_ok) >= 2:
                _last_two = [s.speedup for s in _rewrite_ok[-2:]]
                _plateaued = (max(_last_two) - min(_last_two)) < 0.1 * max(_last_two)
            else:
                _plateaued = False
            if _plateaued:
                guidance += (
                    f"\n\n⚠⚠ 平台期升级：{strided_msg}\n"
                    f"最近两次源码重写收益已趋于平缓（{_last_two[0]:.3f}x → {_last_two[1]:.3f}x，"
                    f"没有实质提升），且从未真正尝试过循环交换。下一次 rewrite_source **必须**用 "
                    f"base=\"original\" 从原始代码重新出发（不要在当前 tiling/分块版本上继续微调），"
                    f"strategy 明确写循环交换：把上面检测到的那对变量互换嵌套顺序，"
                    f"让访存变成连续的（stride-1）。这类结构性修复历史上比继续微调现有方向收益大得多。"
                )
            elif not _rewrite_ok:
                guidance += f"\n\n【静态分析提示】{strided_msg}"

    if remaining <= 2:
        guidance += f"  仅剩 {remaining} 步——请集中在成功率最高的操作。"

    return guidance


def _build_evidence_sections(ev: dict, baseline_perf: dict, missed_lines: list) -> dict:
    """构建 IR 文本、富 remarks、VTune、IR-diff、静态分析段——均为只读展示文本。"""
    # ── IR：只显示 kernel 函数部分，限 80 行 ─────────────────────────────────
    ir_lines = (ev.get("kernel_ir", "") or "").split("\n")
    ir_was_truncated = len(ir_lines) > 80
    if ir_was_truncated:
        trunc_count = len(ir_lines) - 80
        ir_lines = ir_lines[:80] + [
            f"// [TRUNCATED: {trunc_count} lines omitted for context budget]"
        ]
    ir_text = "\n".join(ir_lines)

    # ── 富 YAML remarks（精确到代码行/列/向量化因子/源码片段）─────────────────
    rich_remarks = ev.get("rich_remarks", {})
    rich_missed_str = ""
    if rich_remarks:
        try:
            # format_rich_remarks_for_source_prompt takes the full dict
            rich_missed_str = format_rich_remarks_for_source_prompt(rich_remarks, max_missed=13)
        except Exception:
            rich_missed_str = "\n".join(missed_lines) if missed_lines else "  (无 missed)"
    else:
        # fallback to simple text remarks
        rich_missed_str = "\n".join(missed_lines) if missed_lines else "  (无 missed)"

    # ── VTune 补充段（有数据时才加入）────────────────────────────────────────
    vtune_section = ""
    vtune_mb = baseline_perf.get("vtune_memory_bound_pct")
    if vtune_mb is not None or baseline_perf.get("vtune_top_hotspots"):
        try:
            from src.vtune_analysis import format_vtune_summary
            vtune_section = f"\n【VTune Top-Down 深度分析】\n{format_vtune_summary(baseline_perf)}"
        except Exception:
            pass

    # ── IR diff per-pass 表（有数据时加入）───────────────────────────────────
    ir_diff_pass_str = ""
    ir_pass_diffs = ev.get("ir_pass_diffs", {})
    if ir_pass_diffs:
        try:
            from src.ir_diff import format_ir_diff_table
            ir_diff_pass_str = format_ir_diff_table(ir_pass_diffs)
        except Exception:
            pass

    # ── 静态分析段（无 polybench 数据时替换 perf 段）────────────────────────
    static_summary = ev.get("static_summary", "")
    is_static = ev.get("is_static_fallback", False)
    static_note_str = (f"【注意：无 PolyBench 运行时数据，以下为静态分析推断】\n{static_summary}"
                       if is_static else "")

    return {
        "ir_lines":         ir_lines,
        "ir_text":          ir_text,
        "rich_missed_str":  rich_missed_str,
        "vtune_section":    vtune_section,
        "ir_diff_pass_str": ir_diff_pass_str,
        "static_summary":   static_summary,
        "is_static":        is_static,
        "static_note_str":  static_note_str,
    }


def _current_hotspot_context(ev: dict, base_src_text: str,
                             current_best_source: "str | None" = None) -> tuple:
    """Current body text of the cached hotspot target (ev["hotspot_target"],
    set once in main() right after evidence collection -- NOT re-selected
    here; which function is "the real hotspot" is a structural fact about
    the call graph that doesn't change mid-run). Re-extracts fresh on every
    call so accepted rewrites are reflected: a driver-level current_best_source
    if the target is kernel_name itself, or the (possibly already-rewritten
    and persisted -- see _eval_utils_rewrite) utils/polybench.c if the target
    lives there. Returns (target_name, body_text, in_utils).
    """
    kernel_name = ev["kernel_name"]
    target_name = ev.get("hotspot_target", kernel_name)
    if target_name == kernel_name:
        return target_name, (current_best_source or base_src_text), False
    if ev.get("hotspot_in_utils") and ev.get("utils"):
        pc = Path(ev["utils"]) / "polybench.c"
        text = None
        if pc.exists():
            try:
                text = pc.read_text(errors="replace")
            except OSError:
                pass
        body, _, _ = extract_kernel_function(text, target_name) if text else (None, None, None)
        return target_name, (body or ""), True
    body, _, _ = extract_kernel_function(current_best_source or base_src_text, target_name)
    return target_name, (body or ""), False


def _current_hotspot_contexts(ev: dict, base_src_text: str,
                              current_best_source: "str | None" = None) -> list:
    """Plural sibling of _current_hotspot_context(): current body text for
    EVERY name in ev["hotspot_targets"] (set once in main() via
    select_hotspot_targets() -- see its docstring for when this is more
    than one name). Returns a list of (name, body, in_utils) tuples in the
    same order as ev["hotspot_targets"]; falls back to a single-element
    list matching _current_hotspot_context's result when there's only one
    target (the overwhelmingly common case), so callers that only care
    about "the" target can keep using the singular function unchanged.
    """
    kernel_name = ev["kernel_name"]
    names = ev.get("hotspot_targets") or [ev.get("hotspot_target", kernel_name)]
    if len(names) <= 1:
        return [_current_hotspot_context(ev, base_src_text, current_best_source)]
    in_utils = ev.get("hotspot_in_utils", False)
    text = None
    if in_utils and ev.get("utils"):
        pc = Path(ev["utils"]) / "polybench.c"
        if pc.exists():
            try:
                text = pc.read_text(errors="replace")
            except OSError:
                pass
    else:
        text = current_best_source or base_src_text
    out = []
    for name in names:
        body, _, _ = extract_kernel_function(text, name) if text else (None, None, None)
        out.append((name, body or "", in_utils))
    return out


def _build_agent_prompt(kernel_name: str, ev: dict,
                        cpu_info: str, cpu_cache: str,
                        history: "OptimizationHistory",
                        current_best_source: "str | None",
                        current_best_flags:  list,
                        step_num: int, max_steps: int,
                        forced_action: "str | None" = None) -> str:
    """
    构建 agent 模式的 LLM 决策提示。

    设计原则：
    1. 逆向推断：perf 瓶颈 → 对应 O3 pass → 参数调整
    2. 只展示有 missed remarks 且有可调参数的 pass（过滤噪声）
    3. 上下文压缩：有 token 预算，超出则截断
    4. 历史 perf 趋势：让 LLM 看到每步后指标如何变化
    5. 中文引导，英文数值和代码
    """
    # ── 上下文压缩 ──────────────────────────────────────────────────────────
    # 估算 token（1 token ≈ 4 chars），总输入预算 32000 tokens = 128000 chars
    TOKEN_BUDGET = 28000  # 留 4000 给输出

    pg_summary = ev.get("pass_graph", {}).get("summary", "")
    if not pg_summary:
        pg_summary = f"{len(ev['kernel_passes'])} passes ran on {kernel_name}."

    remarks_info      = _build_remarks_and_targeted_passes(ev)
    missed_lines      = remarks_info["missed_lines"]
    passed_str        = remarks_info["passed_str"]
    perf_summary_str  = remarks_info["perf_summary_str"]
    targeted_pass_str = remarks_info["targeted_pass_str"]
    baseline_perf     = remarks_info["baseline_perf"]
    bottleneck_hints  = baseline_perf.get("bottleneck_hints", ["unknown"])

    # ── pragma 参考 ──────────────────────────────────────────────────────────
    pragma_lines = []
    for ph in PRAGMA_LOOP_HINTS:
        pragma_lines.append(f"  [{ph['group']}] {ph['pragma']}  -- {ph['desc']}")

    # ── 当前最优状态 ──────────────────────────────────────────────────────────
    best_sp   = history.best_speedup
    best_desc = history.best_combo
    flags_str = " ".join(current_best_flags) if current_best_flags else "(无)"
    # Extract current best kernel for display
    current_best_kernel = ""
    if current_best_source:
        _cbk, _, _ = extract_kernel_function(current_best_source, kernel_name)
        current_best_kernel = _cbk or ""
        source_note = ("当前最优是经过源码重写的版本（见下方 Current Best Kernel）。"
                       "可用 base=\"current_best\" 在此基础上继续，或 base=\"original\" 重新开始。")
    else:
        source_note = "尚无源码重写（仅 -mllvm flags 被测试过）。"

    # ── 热点重定向：如果真正该改的函数不是 kernel_name 自己（见 main() 里
    # 选一次、缓存进 ev["hotspot_target"] 的说明），这里必须让 LLM 知道，
    # 并且下面【③ Kernel 源码】要展示真正的目标函数，而不是 kernel_name——
    # 否则 LLM 会照着 kernel_name 的代码提 rewrite_source strategy，但实际
    # 写代码、验证、生效的却是热点函数，两者对不上。
    _hs_contexts = _current_hotspot_contexts(ev, ev['kernel_text'], current_best_source)
    _hs_target, _hs_body, _hs_in_utils = _hs_contexts[0]
    _hs_multi = len(_hs_contexts) > 1
    hotspot_note = ""
    display_kernel_text = ev['kernel_text']
    display_kernel_label = kernel_name
    if _hs_target != kernel_name:
        _hs_where = ("utils/polybench.c（该 run 私有可写副本，rewrite_source 改动会持久化到这里）"
                    if _hs_in_utils else "driver 文件")
        if _hs_multi:
            _hs_names = [n for n, _, _ in _hs_contexts]
            hotspot_note = (
                f"\n⚠ 热点重定向（多函数联合）：`{kernel_name}` 本身只是入口/wrapper，"
                f"真正被反复执行、决定性能的不是单个函数，而是这 {len(_hs_names)} 个热点分数"
                f"彼此接近的函数一起构成的调用链：{', '.join(f'`{n}`' for n in _hs_names)}"
                f"（均位于 {_hs_where}）。判定依据：{ev.get('hotspot_reason', '')}\n"
                f"下面【③ Kernel 源码】依次展示这 {len(_hs_names)} 个函数的完整代码——"
                f"提 rewrite_source 的 strategy 时要把它们当一个整体来设计变换"
                f"（比如内联合并、消除函数间调用开销、跨函数消除冗余计算），"
                f"而不是只改其中一个；不要描述对 `{kernel_name}` 本身的改动。\n"
                f"实现阶段会要求同时输出这 {len(_hs_names)} 个函数的新代码，"
                f"每个前面带一行 `// ===COMET_FUNC: <函数名>===` 标记，顺序和上面展示的一致"
                f"（可以在函数体内部做任意重构，但每个原函数名对应的标记必须都出现一次）。\n")
            display_kernel_text = "\n\n".join(
                f"// ===COMET_FUNC: {n}===\n{b}" for n, b, _ in _hs_contexts)
            display_kernel_label = " + ".join(_hs_names)
        else:
            hotspot_note = (
                f"\n⚠ 热点重定向：`{kernel_name}` 本身只是入口/wrapper，真正被反复执行、"
                f"决定性能的函数是 `{_hs_target}`（位于 {_hs_where}）。"
                f"判定依据：{ev.get('hotspot_reason', '')}\n"
                f"下面【③ Kernel 源码】展示的就是 `{_hs_target}`（不是 `{kernel_name}`）——"
                f"提 rewrite_source 的 strategy 时必须针对这段代码里的具体变量/循环/调用设计变换，"
                f"不要描述对 `{kernel_name}` 本身的改动（比如合并它的 printf、给它的参数校验加分支预测提示），"
                f"因为实际写代码、验证、生效的目标就是 `{_hs_target}`。\n")
            display_kernel_text = _hs_body
            display_kernel_label = _hs_target

    # ── history 部分 ─────────────────────────────────────────────────────────
    history_section = history.to_prompt_section()

    strided_hint = _detect_strided_inner_loop(display_kernel_text)
    guidance = _build_step_guidance(step_num, max_steps, forced_action,
                                    best_sp, best_desc, bottleneck_hints, history,
                                    strided_hint=strided_hint)

    evidence          = _build_evidence_sections(ev, baseline_perf, missed_lines)
    ir_lines          = evidence["ir_lines"]
    ir_text           = evidence["ir_text"]
    rich_missed_str   = evidence["rich_missed_str"]
    vtune_section     = evidence["vtune_section"]
    ir_diff_pass_str  = evidence["ir_diff_pass_str"]
    static_summary    = evidence["static_summary"]
    is_static         = evidence["is_static"]
    static_note_str   = evidence["static_note_str"]

    # 组装各部分字符串
    pragma_str   = "\n".join(pragma_lines)
    ir_diff_str  = ("\n".join(ev['ir_diff_info'])
                    if ev.get('ir_diff_info') else "  (无探针 IR 变化)")
    hist_str     = history_section if history_section else "## History  (第1步，无历史)"

    prompt = f"""你是一名专业的 LLVM 编译器优化工程师，以 agent 模式运行。

目标：通过调整 LLVM O3 内部 pass 参数和/或重写 kernel 源码，
提升 `{kernel_name}` 相对于 -O3 baseline 的运行时性能。

约束：
- 只能调整 O3 pipeline 中已经运行过的 pass 的 cost-model 参数（-mllvm 标志）
- 不能添加新 pass，不能使用 force-*/disable-* 标志绕过 cost model
- 源码修改必须保证与原始输出数值一致（SMALL 和 STANDARD 两个数据集规模均验证）
- 每次只选择一个 action
{hotspot_note}
{guidance}

══════════════════════════════════════════════════════════════
## ① 硬件环境
{cpu_info}
{cpu_cache}

══════════════════════════════════════════════════════════════
## ② 性能计数器基线（perf stat{"+ VTune Top-Down" if vtune_section else ""}）
{perf_summary_str}{vtune_section}
{static_note_str}

【逆向推断】根据以上瓶颈，以下 O3 pass 最值得优先调整：
（只列出：① 确实在 O3 pipeline 中运行过，② 有 missed remarks，③ 有可调 cost-model 参数）
{targeted_pass_str}

══════════════════════════════════════════════════════════════
## ③ Kernel 源码（目标函数：`{display_kernel_label}`）

{"### 当前版本（可能已包含之前步骤被接受的改写；正确性验证始终对比整个程序最初的输出）" if _hs_target != kernel_name else "### ORIGINAL（所有正确性验证均对比此版本输出）"}
```c
{display_kernel_text}
```
{("### Current Best Kernel（" + f"{best_sp:.3f}x" + "，base='current_best' 在此基础上继续）" + chr(10) + "```c" + chr(10) + current_best_kernel + chr(10) + "```") if (current_best_kernel and _hs_target == kernel_name) else ""}
当前最优状态：
  加速比: {best_sp:.3f}x  [{best_desc}]
  最优 flags: {flags_str}
  {source_note}

══════════════════════════════════════════════════════════════
## ④ LLVM O0 IR（kernel 函数，优化前原始 IR）
```llvm
{ir_text}
```

══════════════════════════════════════════════════════════════
## ⑤ O3 Pass Pipeline 分析
{pg_summary}

**Missed pass（有优化意愿但被 cost model 拒绝——精确到代码位置、向量化因子、失败原因）:**
{rich_missed_str}

成功触发的 pass（transformations applied）:
{passed_str}

══════════════════════════════════════════════════════════════
## ⑥ IR 统计 (-O3)
{ev['baseline_stats']}

探针值 IR 变化（修改某 flag 后 vector_ops/instr 的变化量）:
{ir_diff_str}

Per-pass IR 修改情况（每个 pass 单独运行前后对比 — FIRED=确实修改了IR，no-op=运行了但没变化）:
{ir_diff_pass_str if ir_diff_pass_str else "  (IR diff 未采集)"}

══════════════════════════════════════════════════════════════
{hist_str}

══════════════════════════════════════════════════════════════
## 可选 Action

### action: "try_flags"
测试 -mllvm pass cost-model 参数。系统自动测试所有候选值，并尝试最优组合。
  "base": "original" | "current_best"   -- 使用哪个源码版本编译
  "flags": [{{"flag": "-<flag-name>", "candidates": [<val1>,<val2>,...], "rationale": "..."}}]

⚠ 重要：参数是与源码独立的维度。
  - 每次 rewrite_source 成功后，应立即用 base="current_best" 重新做 try_flags，
    因为新源码的 IR 结构不同，最优参数可能与原始不同。
  - 系统会自动用 current_best_flags 测 source+flags 组合，但 try_flags 能更精确地搜参数。
  - 参数和源码的最佳组合往往比单独优化效果好得多。

候选值生成规则（由你根据逆向推断中的参数描述自行推断）：
  - 参数描述中通常包含 "default = N"；候选值应覆盖"更激进"方向
  - uint 阈值（如 licm-max-num-uses-traversed, default=8）：试 default*2, *4, *8, *16
  - int 阈值（如 slp-threshold, default=0）：试负值（-1,-2,-4,-8,5,10,20——负值降低 cost bar，更激进）
  - 无 default 的 uint：试 2,4,8,16,32,64 中的代表值
  - 每个 flag 生成 4-8 个候选值；包含 default 作为参照基准

重要规则：
  - 只能使用逆向推断部分列出的 flag（均已确认存在于此 clang 版本，类型和描述见下方）
  - 不能使用 force-vector-width, force-vector-interleave 等 force-*/disable-* 标志
  - 尽量覆盖广：一次性提出 10-20 个 flag（宁多勿少——每个 flag 的候选值搜索是独立的，
    系统会自动跳过编译过慢的 flag，不会拖慢整体），每个 flag 生成 4-8 个候选值。
    这是本系统的核心能力：靠 compiler remarks + missed-optimization 证据，一次性圈定
    一批真正有依据的候选 flag，再由系统快速网格搜索找出最优组合——比人工调参覆盖面更广、
    比盲目枚举全部 flag 更精准。

可用 pragma:
{pragma_str}

### action: "try_pragma"
在特定 for 循环前插入 #pragma clang loop 元数据，影响该循环的 cost model 决策。
  "base": "original" | "current_best"
  "pragma_hints": [{{"loop_prefix": "for (int i = ...", "pragma": "#pragma clang loop ..."}}]
  "also_flags": []   -- 可选：同时应用的 flags（扁平列表，如 ["-mllvm", "-<flag>=<val>"]）

规则：只对无循环携带依赖的循环使用；绝不对 in-place stencil/三角求解/Gauss-Seidel 使用向量化 pragma。

### action: "rewrite_source"
重写 kernel 源码（可多次，每次都验证正确性）。
  "base": "original" | "current_best"   -- 从哪个版本开始改
  "strategy": "一句话描述具体要做什么变换（要精准：哪个循环、哪个变量、为什么）"
  "also_flags": []   -- 可选：同时测试哪些 flags

  注意：不要在此 JSON 中输出 kernel_code。
  系统会先运行专项瓶颈分析 LLM（读取全部 perf/remarks/IR 证据），
  然后由实现 LLM 根据分析结论写代码。你只需给出 strategy 和 base。

允许的变换：循环 tiling/cache blocking、寄存器 blocking（scalar accumulator）、
循环交换、循环融合/分裂、标量提升、O(tile²) 临时 scratch buffer。

⚠ 重要：不要把"外层循环有串行依赖，无法并行/向量化"和"这个 kernel 没有优化空间"
混为一谈——这是两回事。cache tiling/blocking 优化的是内层计算的访存局部性
（减少 cache miss、减少重复从内存读取的数据量），并不需要打破外层的串行依赖：
外层循环该按什么顺序执行还是按什么顺序执行，只是把内层循环重组成分块处理，
让每一块的数据能留在 cache 里被复用。Cholesky/LU/QR 分解这类"外层列/行之间
确实是串行依赖"的稠密线性代数 kernel，在真实 HPC 实践里恰恰是靠分块
（blocked Cholesky/blocked LU）拿到最大加速的——串行依赖从来不是"不能分块"
的理由，只是"不能跨块并行"。如果一个 kernel 有嵌套循环遍历 2D/3D 数组、
且从未尝试过 tiling/blocking，不要因为"外层依赖"就判定它没有优化空间。

禁止：
  - 分裂归约 accumulator（导致 FP 重排 → 数值不一致）
  - 对 in-place kernel 做 Jacobi-style 数组复制（改变算法语义）
  - 添加向量化提示到存在循环依赖的循环

### action: "done"
终止优化。用于：已收敛、kernel 存在无法克服的根本瓶颈、或预算耗尽。
  "reason": "终止原因"

⚠ 在选择 "done" 之前自查：如果这个 kernel 有嵌套循环遍历 2D/3D 数组
（典型的稠密线性代数/stencil 计算），而 rewrite_source 从未真正尝试过
tiling/cache blocking（哪怕只试了 scalar accumulator 或标量提升这类小改动），
不要现在终止——tiling 往往是这类 kernel 收益最大的单一变换，"外层有串行
依赖"不构成跳过它的理由（见上面 rewrite_source 部分的说明）。先用剩余步骤
认真试一次分块，不行再终止。

══════════════════════════════════════════════════════════════
## 输出格式（严格 JSON，无 markdown，无前言后语）

{{
  "reasoning": "<2-4句：从 perf/IR/missed remarks 证据出发，解释为何选这个 action>",
  "improvement_analysis": "<必填：若历史中有过改进，分析为什么改进不够大（哪个瓶颈仍未克服？是内存带宽上限、串行依赖、别名分析失败、还是 cost model 过于保守？）；若从未有过改进，解释根本原因（循环依赖、数据集太小导致计时器噪声底层、访存模式无法cache-friendly化等）>",
  "action": "try_flags",
  "base": "original",
  "flags": [{{"flag": "-<flag-name>", "candidates": [<val1>,<val2>,<default>], "rationale": "..."}}]
}}

只输出与选定 action 相关的字段。improvement_analysis 每次都必须包含。
"""

    # ── 上下文 budget 检查：超出则按优先级截断 ────────────────────────────────
    # 截断顺序（越靠前越先截断，对 LLM 价值最低的先去掉）：
    #   1. static_summary（有 perf 数据时冗余）
    #   2. ir_diff_pass_str（per-pass IR 表，补充信息）
    #   3. vtune_section（重要但 perf 已有覆盖）
    #   4. IR 文本（从 80→40 行）
    prompt_chars = len(prompt)
    if prompt_chars > TOKEN_BUDGET * 4:
        # 尝试截断 static_summary（如果非 static 模式）
        if static_summary and not is_static and static_summary in prompt:
            prompt = prompt.replace(static_summary, "  (static_summary omitted)")
        if len(prompt) > TOKEN_BUDGET * 4 and ir_diff_pass_str and ir_diff_pass_str in prompt:
            prompt = prompt.replace(ir_diff_pass_str, "  (ir_diff_table omitted for budget)")
        if len(prompt) > TOKEN_BUDGET * 4 and vtune_section and vtune_section in prompt:
            prompt = prompt.replace(vtune_section, "")
        if len(prompt) > TOKEN_BUDGET * 4:
            # 最后截短 IR
            short_ir = ("\n".join(ir_lines[:40]) +
                        f"\n// [TRUNCATED: further reduced to 40 lines for context budget]")
            prompt = prompt.replace(ir_text, short_ir)

    return prompt


def _apply_pragma_hints(source: str, hints: list) -> str:
    """
    Insert #pragma clang loop annotations before matching for-loop lines.
    hints: list of {loop_prefix, pragma}

    Matching strategy, three tiers tried in order (looser each time):
      1. Normalised token match: remove all spaces then compare first 60 chars
         (handles `i < m` vs `i<m`, `++i` vs `i++` mismatches)
      2. Original prefix match: first 50 chars of stripped line vs stripped prefix
      3. Keyword fallback: all non-whitespace words in prefix appear on line

    Within a tier, a match is only accepted if it's UNIQUE. PolyBench-shaped
    kernels routinely have multiple loop nests with identical or
    near-identical headers (e.g. 2mm.c has `for (i = 0; i < ni; i++)` /
    `for (j = 0; j < nl; j++)` appearing twice, for two completely
    different loops -- an init/scale pass and the real accumulation loop).
    The old code took the first match at any tier regardless of how many
    other lines equally qualified, which can silently attach a pragma
    (including vectorize(enable), which the caller's rules explicitly
    forbid on loops with carried dependencies) to the wrong loop with no
    indication anything went wrong. A tier with >1 candidate is now
    treated as ambiguous and rejected outright -- it does NOT fall through
    to a looser tier, since a looser tier is even more likely to multi-match.
    """
    import re as _re

    def _norm(s: str) -> str:
        return _re.sub(r'\s+', '', s)

    lines = source.split("\n")
    insertions: list = []  # (line_idx, pragma_text)
    unmatched = []
    for h in hints:
        prefix = h.get("loop_prefix", "").strip()
        pragma = h.get("pragma", "").strip()
        if not prefix or not pragma:
            continue
        norm_prefix = _norm(prefix)[:60]
        words_prefix = set(_re.findall(r'\w+', prefix))

        tier1, tier2, tier3 = [], [], []
        for i, line in enumerate(lines):
            stripped = line.strip()
            if not stripped.startswith("for"):
                continue
            if _norm(stripped)[:60].startswith(norm_prefix):
                tier1.append(i)
            elif stripped[:50].startswith(prefix[:50]):
                tier2.append(i)
            elif words_prefix and words_prefix.issubset(
                    set(_re.findall(r'\w+', stripped))):
                tier3.append(i)

        matched_idx = None
        for tier_name, tier in (("normalized-prefix", tier1),
                                ("prefix", tier2), ("keyword-subset", tier3)):
            if len(tier) == 1:
                matched_idx = tier[0]
                break
            if len(tier) > 1:
                print(f"  [pragma匹配] ⚠ \"{prefix[:50]}\" 在 {tier_name} 档命中 "
                      f"{len(tier)} 处循环（行 {[i + 1 for i in tier]}），有歧义，"
                      f"拒绝盲猜，跳过这条 pragma")
                break  # don't fall through to a looser tier -- more candidates, not fewer
        if matched_idx is not None:
            insertions.append((matched_idx, pragma))
        else:
            unmatched.append(prefix[:60])

    if unmatched:
        print(f"  [pragma匹配] {len(unmatched)}/{len(hints)} 条 pragma 未找到唯一匹配的循环：{unmatched}")

    if not insertions:
        return source

    # Insert in reverse order so indices stay valid
    for idx, pragma in sorted(insertions, key=lambda x: -x[0]):
        indent = len(lines[idx]) - len(lines[idx].lstrip())
        lines.insert(idx, " " * indent + pragma)
    return "\n".join(lines)


def _infer_candidates_from_desc(flag: str, opt_type: str, desc: str) -> list:
    """
    Infer candidate values to explore for a numeric LLVM flag based on its
    description from opt --help-hidden (which often states the default).

    Strategy: parse "default = N" or "(default = N)" from description,
    then generate candidates in the MORE AGGRESSIVE direction.
    For uint thresholds: larger = more opt, try 2x/4x/8x/16x of default.
    For int thresholds:  negative = more opt (lower cost bar), try negatives.
    For unlimited (-1 default): try capping at lower positive values.
    """
    import re as _re

    # Extract default value from description
    default_val = None
    m = _re.search(r'default\s*[=:]?\s*(-?\d+)', desc, _re.I)
    if m:
        default_val = int(m.group(1))

    typ = opt_type.lower()

    if typ == "int":
        # int flags often use negative = more aggressive (lower cost barrier)
        if default_val is not None:
            if default_val == 0:
                # e.g. -slp-threshold=0: try negatives (more aggressive) and positives
                return [-1, -2, -4, -8, 5, 10, 20]
            elif default_val > 0:
                # e.g. default=10: try larger, smaller, and negative
                return [default_val * 2, default_val * 4,
                        max(1, default_val // 2), -1, -2]
            else:
                # default is negative or -1 (unlimited)
                return [default_val, -1, 0]
        else:
            return [-1, -2, -4, 0, 5, 10]

    elif typ == "uint":
        # uint flags: larger = more aggressive (higher limits, more unrolling, etc.)
        if default_val is not None and default_val > 0:
            return [default_val * 2, default_val * 4,
                    default_val * 8, default_val * 16]
        elif default_val == 0:
            # 0 often means "auto"; try small positive values
            return [2, 4, 8, 16]
        else:
            # No default found; use generic range
            return [8, 16, 32, 64, 128, 256]

    else:
        return [1, 2, 4, 8]


# ── Pre-rewrite analysis LLM ─────────────────────────────────────────────────

_REWRITE_ANALYSIS_SYSTEM = """\
You are a compiler performance engineer specializing in LLVM, cache architecture, and \
numerical C optimization. Your task is to DIAGNOSE the bottleneck — not write code. \
Be specific: reference exact variable names, line numbers, pass names, and memory access patterns. \
Reason from the evidence provided. Do not list generic principles; apply them to this specific kernel."""


def _static_pattern_warnings(kernel_txt: str) -> str:
    """
    Run the static pattern detectors (tune_source.py's
    _detect_triangular_loop/_detect_inplace_stencil/
    _detect_inplace_factorization) against the actual kernel text and
    render an explicit, programmatic warning block for the rewrite
    prompts. These detectors already existed and were reasonably reliable
    (regex-based structural checks, not guesses) but were only ever wired
    into the legacy analyze_kernel_patterns() path (--param-only/
    --source-only, not used by the default agent-mode pipeline that
    produces every real result this system reports) -- meaning the live
    rewrite_source prompt has been relying entirely on the LLM correctly
    recognizing "this is a stencil" / "this is in-place LU" from prose
    rules and the raw C text alone, with no programmatic check backing it
    up. Surfacing the same detectors' output directly in the prompt turns
    "please don't do X to stencils" from an instruction the LLM might miss
    into a concrete, kernel-specific fact it's told upfront.
    """
    warnings = []
    if _detect_triangular_loop(kernel_txt):
        warnings.append(
            "- 检测到三角循环边界（如 j<=i 或 i>=j）：这类循环的迭代次数依赖外层索引，"
            "禁止假设它是矩形循环去做简单的 tiling/交换，需要专门处理边界。")
    if _detect_inplace_stencil(kernel_txt):
        warnings.append(
            "- 检测到 in-place stencil 模式（数组以 ±1 偏移原地读写，如 Gauss-Seidel/ADI "
            "column sweep）：更新顺序是算法语义的一部分，绝对禁止 Jacobi 式数组复制或改变"
            "遍历顺序的向量化提示。")
    if _detect_inplace_factorization(kernel_txt):
        warnings.append(
            "- 检测到 in-place LU/Cholesky 分解模式（同一矩阵在同一循环体内被读又被写，如 "
            "A[i][j] -= A[i][k]*A[k][j]）：禁止拆分归约 accumulator（改变浮点运算顺序会导致"
            "数值不一致），tiling 仅可作用于访存局部性，不能改变元素更新的先后顺序。")
    _strided_found, _strided_msg = _detect_strided_inner_loop(kernel_txt)
    if _strided_found:
        warnings.append("- " + _strided_msg)
    if not warnings:
        return ""
    return ("\n### 静态模式检测（程序自动识别，不是猜测——发现即视为硬约束）\n"
           + "\n".join(warnings) + "\n")


def _build_rewrite_analysis_prompt(kernel_name: str, ev: dict,
                                   history: "OptimizationHistory",
                                   base_src_text: str,
                                   strategy: str) -> str:
    """Build prompt for the pre-rewrite bottleneck analysis LLM."""
    from src.remarks import format_rich_remarks_for_source_prompt as _fmt_rich

    # Extract just the kernel function body from the resolved source
    try:
        kernel_txt, _, _ = extract_kernel_function(base_src_text, kernel_name)
        if not kernel_txt:
            kernel_txt = base_src_text
    except Exception:
        kernel_txt = base_src_text

    pattern_warnings = _static_pattern_warnings(kernel_txt)

    # ── remarks/IR 是不是真的在描述这次要改的函数？──────────────────────────
    # ev["kernel_remarks"]/ev["rich_remarks"]/ev["kernel_ir"] 都是 evidence
    # collection 阶段一次性针对入口函数（ev["kernel_name"]）算出来的。
    # src/hotspot.py 接进来之后，这次要改的 kernel_name 参数有时候是入口函数
    # 调用链更深处的一个函数（比如 mcf_r 的 price_out_impl，不是 kernel_mcf_r
    # 本身）——这种情况下继续把入口函数的 remarks/IR 当成"这个函数的证据"塞进
    # prompt 里，是在喂错误的上下文：LLM 会以为这些 missed remarks 是它正在
    # 改的函数产生的，实际上跟目标函数毫无关系。
    _target_is_entry = (kernel_name == ev.get("kernel_name"))

    # Rich remarks
    rich_remarks = ev.get("rich_remarks", {})
    remarks_str = ""
    if _target_is_entry and rich_remarks:
        try:
            remarks_str = _fmt_rich(rich_remarks, max_missed=15)
        except Exception:
            pass
    if _target_is_entry and not remarks_str:
        # Fallback to regular remarks
        rl = []
        for pname, entries in ev.get("kernel_remarks", {}).items():
            misses = [e for e in entries if e["type"] == "missed"]
            if misses:
                rl.append(f"  {pname}: {len(misses)} missed")
                for e in misses[:3]:
                    rl.append(f"    line {e['line']}: {e['msg'][:100]}")
        remarks_str = "\n".join(rl) if rl else "  (none)"
    if not _target_is_entry:
        remarks_str = (f"  (not available -- these compiler remarks were collected for the entry "
                       f"function {ev.get('kernel_name')!r}, not for {kernel_name!r}, which is the "
                       f"actual target here. Reason it was selected: {ev.get('hotspot_reason', '')})")

    # Perf counters
    baseline_perf = ev.get("baseline_perf", {})
    ipc       = baseline_perf.get("ipc")
    l1_miss   = baseline_perf.get("l1_miss_rate")
    llc_miss  = baseline_perf.get("llc_miss_rate")
    bot_hints = baseline_perf.get("bottleneck_hints", [])
    perf_lines = []
    if ipc is not None:
        perf_lines.append(f"  IPC: {ipc:.2f}  [<1.0 = memory-bound or serial dependency]")
    if l1_miss is not None:
        perf_lines.append(f"  L1-dcache miss rate: {l1_miss:.1f}%")
    if llc_miss is not None:
        perf_lines.append(f"  LLC miss rate: {llc_miss:.1f}%")
    if bot_hints:
        perf_lines.append(f"  Bottleneck hints: {', '.join(bot_hints)}")
    perf_str = "\n".join(perf_lines) if perf_lines else "  (not available)"

    # IR excerpt (brief — full IR too long). Same entry-vs-target caveat as
    # the remarks above -- ev["kernel_ir"] is the entry function's IR only.
    ir_section = ""
    if not _target_is_entry:
        ir_section = (f"\n## LLVM IR excerpt\n(not available -- this is the entry function "
                      f"{ev.get('kernel_name')!r}'s IR, not {kernel_name!r}'s; skipped rather "
                      f"than shown as if it described the actual target)\n")
    else:
        ir_text = (ev.get("kernel_ir", "") or "")[:2500]
        if ir_text:
            ir_section = f"\n## LLVM IR excerpt (first ~2500 chars of -O3 IR)\n```llvm\n{ir_text}\n```\n"

    # Failed rewrite history
    failed_rewrites = [s for s in history.steps
                       if s.action == "rewrite_source" and (s.error or s.speedup < 0.99)]
    failed_block = ""
    if failed_rewrites:
        lines = ["## Previously attempted rewrites that FAILED or REGRESSED",
                 "## (Do NOT repeat the SAME strategy, but a DIFFERENT approach is encouraged)"]
        for s in failed_rewrites[-4:]:
            status = f"error: {s.error[:80]}" if s.error else f"regressed to {s.speedup:.3f}x"
            lines.append(f"  - {s.strategy}: {status}")
        failed_block = "\n".join(lines) + "\n"

    return f"""Diagnose the performance bottleneck in `{kernel_name}` before an optimization attempt.

## Proposed optimization strategy
"{strategy}"

## Current kernel source (version to optimize)
```c
{kernel_txt}
```
{pattern_warnings}
## Hardware performance counters (measured on current -O3 build)
{perf_str}

## LLVM optimization remarks (what the compiler tried and MISSED under -O3)
{remarks_str}
{ir_section}
{failed_block}
## Your diagnosis task

Work through the evidence carefully and produce a targeted, specific diagnosis.

**Step 1 — Memory access pattern**
For each array accessed in the innermost loops:
- What is the access stride? (stride-1 / stride-N / random)
- How many unique values exist per outer-loop iteration vs how many loads are issued?
- Does this access pattern cause cache pressure? Why?

**Step 2 — Missed compiler optimizations**
For each missed remark:
- What transformation did the compiler attempt?
- Why did it fail? (alias analysis, cost model, data dependency, etc.)
- Can a source-level change unlock this optimization? How?

**Step 3 — Root cause**
State the PRIMARY bottleneck as precisely as possible:
  - Which exact load/store/computation is the hot path?
  - What evidence (perf counter, remark, IR pattern) confirms this?
  - How does this interact with the loop structure?

**Step 4 — Critically evaluate the proposed strategy**
The planner suggested: "{strategy}"

Assume this might be WRONG. Check whether it actually addresses the root cause from Step 3.

Answer these questions:
- Does this strategy directly fix the bottleneck you identified? Show the causal chain.
- What is the RISK of this approach? Consider: loop overhead, vectorization disruption,
  increased register pressure, FP reordering, working set changes, alias analysis breakage.
- Does the measured evidence actually support this strategy?
  (IPC={ev.get('baseline_perf', {}).get('ipc', '?')},
   L1-miss={ev.get('baseline_perf', {}).get('l1_miss_rate', '?')}%,
   LLC-miss={ev.get('baseline_perf', {}).get('llc_miss_rate', '?')}%)
  A strategy that targets cache locality only pays off when LLC/L1 misses are actually high.
  A strategy that targets vectorization only pays off when the remarks show vectorization failures.
  A strategy that targets ILP only pays off when IPC is low due to dependency chains, not memory stalls.
- Is there a SIMPLER, more targeted fix that addresses the root cause with less risk?

**Step 5 — Final recommendation**
Make a binary decision:
  A) CONFIRM the proposed strategy — state exactly how to implement it to avoid the risks above.
  B) REJECT the proposed strategy — state the specific reason it won't work for THIS kernel's
     bottleneck, and give a concrete ALTERNATIVE (name the exact loop, variable, or remark to fix).

Do not hedge. Pick A or B and give a precise implementation plan.
Failed rewrites to absolutely avoid: {[s.strategy for s in history.steps if s.action == 'rewrite_source' and (s.error or s.speedup < 0.99)]!r}
"""


def analyze_rewrite_bottleneck(llm, kernel_name: str, ev: dict,
                               history: "OptimizationHistory",
                               base_src_text: str,
                               strategy: str,
                               max_tokens: int = 2048) -> str:
    """
    Pre-rewrite analysis LLM: diagnoses the specific bottleneck before code generation.
    Returns a diagnosis string, or "" on failure.
    """
    import time, random
    prompt = _build_rewrite_analysis_prompt(
        kernel_name, ev, history, base_src_text, strategy)
    for attempt in range(3):
        if attempt > 0:
            time.sleep(min(2 ** attempt, 8) + random.uniform(0, 1))
        try:
            resp = llm.call(
                [{"role": "system", "content": _REWRITE_ANALYSIS_SYSTEM},
                 {"role": "user",   "content": prompt}],
                max_tokens=max_tokens,
                timeout=120,
                temperature=0,
            )
            if resp and resp.strip():
                return resp.strip()
        except Exception:
            pass
    return ""


# ── Meta-planner: action sequence planning ───────────────────────────────────

def plan_action_sequence(llm, kernel_name: str,
                         history: "OptimizationHistory",
                         step_num: int, max_steps: int,
                         max_tokens: int = 512) -> List[str]:
    """
    元规划 LLM：根据当前历史决定接下来最多 3 步应该用哪些工具，
    保证多个工具交替出现，避免同一工具连续重复。

    返回行动序列列表（如 ["try_pragma", "try_flags", "rewrite_source"]），
    每个元素都是合法 action 名称。失败时返回空列表。
    """
    remaining = max_steps - step_num + 1
    plan_count = min(3, remaining)
    if plan_count <= 0:
        return []

    action_counts: Dict[str, int] = {}
    for s in history.steps:
        if s.action not in ("done", "error"):
            action_counts[s.action] = action_counts.get(s.action, 0) + 1

    recent_actions = [s.action for s in history.steps[-5:]
                      if s.action not in ("done", "error")]
    recent_speedups = [f"{s.speedup:.3f}x" for s in history.steps[-4:] if not s.error]

    consecutive_same = 0
    if history.steps:
        last = history.steps[-1].action
        for s in reversed(history.steps):
            if s.action == last:
                consecutive_same += 1
            else:
                break

    done_flags   = any(s.action == "try_flags"      and not s.error for s in history.steps)
    done_rewrite = any(s.action == "rewrite_source" and not s.error and s.speedup >= 1.0
                       for s in history.steps)
    done_pragma  = any(s.action == "try_pragma"     and not s.error for s in history.steps)

    prompt = f"""You are a meta-planner for a compiler optimization agent targeting `{kernel_name}`.

Current status:
  steps done: {len(history.steps)}, remaining: {remaining}, best speedup: {history.best_speedup:.3f}x
  action counts: try_flags={action_counts.get('try_flags',0)}, rewrite_source={action_counts.get('rewrite_source',0)}, try_pragma={action_counts.get('try_pragma',0)}
  recent actions (oldest→newest): {recent_actions}
  consecutive same action: {consecutive_same}
  recent speedups: {recent_speedups}
  coverage: flags_tried={done_flags}, rewrite_succeeded={done_rewrite}, pragma_tried={done_pragma}

Planning rules (STRICT):
1. NEVER repeat the same action more than once in your plan.
2. Include try_pragma if it has not yet been tried ({not done_pragma}).
3. After a rewrite_source success, next should be try_flags (re-tune params on new source).
4. After try_flags, prefer rewrite_source or try_pragma to explore orthogonal dimensions.
5. Spread across all three tools: try_flags, rewrite_source, try_pragma.

Output ONLY strict JSON, no prose:
{{"analysis": "<1 sentence why>", "plan": ["<action1>", "<action2>", "<action3>"]}}
Valid action names: try_flags, rewrite_source, try_pragma"""

    try:
        resp = llm.call(
            [{"role": "system",
              "content": "Compiler optimization meta-planner. Output strict JSON only."},
             {"role": "user", "content": prompt}],
            max_tokens=max_tokens,
            temperature=0,
            timeout=60,
        )
        if not resp:
            print("  [Planner] 规划失败（非致命）: LLM 无响应")
            return []
        parsed = json.loads(strip_json_fences(resp))
        raw_plan = parsed.get("plan", [])
        valid = [a if isinstance(a, str) else a.get("action", "")
                 for a in raw_plan]
        valid = [a for a in valid if a in ("try_flags", "rewrite_source", "try_pragma")]
        if valid:
            print(f"  [Planner] {parsed.get('analysis', '')}")
            print(f"  [Planner] 规划序列: {valid}")
            return valid[:plan_count]
    except Exception as _e:
        # 打印原始响应（截断）方便排查是 JSON 格式问题还是 LLM 输出异常
        _raw = (locals().get("resp") or "")[:200]
        print(f"  [Planner] 规划失败（非致命）: {_e}  原始响应: {_raw!r}")
    return []


def _anti_repeat_forced(history: "OptimizationHistory") -> "str | None":
    """
    若同一 action 连续出现 ≥2 次，强制轮换到下一个工具。
    轮换顺序：try_flags → rewrite_source → try_pragma → try_flags → …
    """
    if len(history.steps) < 2:
        return None
    last_action = history.steps[-1].action
    if last_action in ("done", "error"):
        return None
    consecutive = 0
    for s in reversed(history.steps):
        if s.action == last_action:
            consecutive += 1
        else:
            break
    if consecutive < 2:
        return None
    rotation = ["try_flags", "rewrite_source", "try_pragma"]
    if last_action in rotation:
        next_action = rotation[(rotation.index(last_action) + 1) % len(rotation)]
    else:
        next_action = "try_flags"
    print(f"  [反重复] '{last_action}' 已连续 {consecutive} 次，强制切换到 '{next_action}'")
    return next_action


def _widen_span_for_proto_guard(text: str, start_idx: int) -> int:
    """If `start_idx` (extract_kernel_function's span start) lands right
    after a `#ifdef _PROTO_` guard line, widen the span to include that
    guard line too -- otherwise splicing raw_kernel (a single ANSI-style
    signature, per _extract_original_signature's instruction to the LLM;
    never the `#ifdef/#else/#endif` dual-declaration wrapper) into
    [start_idx:end_idx] removes the `#else <K&R decl> #endif` half (which
    IS inside that span, sitting between the signature and the opening
    `{`) but leaves the `#ifdef _PROTO_` line dangling with no matching
    #endif -- exactly the "unterminated conditional directive" compile
    failure observed live on SPEC mcf_r's primal_iminus (utils/
    polybench.c:2513). This SPEC-era code repeats that
    `#ifdef _PROTO_ <ansi> #else <k&r> #endif` idiom throughout, so this
    isn't specific to one function. Returns the original start_idx
    unchanged if no such guard is found immediately before it.
    """
    before = text[:start_idx]
    stripped = before.rstrip()
    marker = "#ifdef _PROTO_"
    if stripped.endswith(marker):
        return len(stripped) - len(marker)
    return start_idx


_COMET_FUNC_MARKER_RE = re.compile(r'//\s*===COMET_FUNC:\s*(\S+?)\s*===\s*\n')


def _splice_multi_spans(text: str, spans: "list[tuple]", replacements: dict) -> "str | None":
    """Multi-function sibling of the single-span splice used everywhere
    else in this file (`text[:start] + raw_kernel + text[end:]`). Removes
    every (name, start, end) span in `spans` from `text` and inserts each
    span's replacement (from `replacements[name]`) at that same original
    position, so unrelated code between the target functions is left
    exactly where it was -- this is NOT "delete everything and paste one
    combined blob at the top", each function's new body lands where its
    old one used to be, in the original file's function ORDER, not
    necessarily the LLM's response order (parse_multi_func_response
    already re-keys by name so this doesn't depend on the LLM preserving
    order either). Returns None if any name in `spans` is missing from
    `replacements` (caller should treat that as a parse failure, not
    silently drop a function).
    """
    for name, _, _ in spans:
        if name not in replacements:
            return None
    ordered = sorted(spans, key=lambda s: s[1])
    out = []
    prev_end = 0
    for name, start, end in ordered:
        widened_start = _widen_span_for_proto_guard(text, start)
        out.append(text[prev_end:widened_start])
        out.append(replacements[name])
        prev_end = end
    out.append(text[prev_end:])
    return "".join(out)


def _parse_multi_func_response(raw: str, expected_names: "list[str]") -> "dict | None":
    """Split an implementation LLM's response into {name: body} using the
    `// ===COMET_FUNC: <name>===` markers _build_rewrite_impl_prompt_multi
    asks for. Returns None (parse failure -- caller reports this as a
    rewrite_source error, same as any other malformed-response case) unless
    EVERY name in expected_names appears exactly once; a partial/renamed/
    reordered response is not silently accepted with the others left
    unchanged, since "some of the N functions got rewritten, the rest
    quietly kept their old text" is a correctness trap, not a valid result.
    """
    matches = list(_COMET_FUNC_MARKER_RE.finditer(raw))
    if not matches:
        return None
    chunks = {}
    for i, m in enumerate(matches):
        name = m.group(1)
        body_start = m.end()
        body_end = matches[i + 1].start() if i + 1 < len(matches) else len(raw)
        chunks[name] = raw[body_start:body_end].strip() + "\n"
    if set(chunks.keys()) != set(expected_names):
        return None
    return chunks


def _build_rewrite_impl_prompt_multi(target_names: "list[str]", ev: dict,
                                     history: "OptimizationHistory",
                                     base_src_text: str,
                                     strategy: str,
                                     analysis_diagnosis: str) -> str:
    """Multi-function sibling of _build_rewrite_impl_prompt(): shows all of
    `target_names`' current bodies together and asks for a combined
    response covering all of them, marked so _parse_multi_func_response can
    split it back apart. Deliberately a separate, smaller function rather
    than parameterizing the single-target builder -- that one is large,
    mature, and heavily exercised; bolting a list-of-targets code path
    through every section of it risked destabilizing the common (single-
    target) case for a feature that only fires when hotspot scores cluster
    within closeness_pct of each other (see select_hotspot_targets)."""
    funcs = []
    for name in target_names:
        body, _, _ = extract_kernel_function(base_src_text, name)
        body = body or ""
        sig = _extract_original_signature(body, name)
        warn = _static_pattern_warnings(body)
        funcs.append((name, body, sig, warn))

    forbidden_macros = ev.get("forbidden_macros", [])
    forbidden_str = ", ".join(f"`{m}`" for m in forbidden_macros[:30]) if forbidden_macros else "none"
    cpu_info = ev.get("cpu_info", "")
    cpu_cache = ev.get("cpu_cache", "")

    diagnosis_block = ""
    if analysis_diagnosis:
        diagnosis_block = (f"### Expert performance analysis (of the overall hot call chain "
                           f"-- read this carefully before writing)\n{analysis_diagnosis}\n\n")

    funcs_block = "\n\n".join(
        f"#### Function {i+1}/{len(funcs)}: `{name}`\n"
        + (f"Original signature (keep EXACTLY, including storage class/linkage): `{sig}`\n" if sig else "")
        + (warn if warn else "")
        + f"```c\n{body}\n```"
        for i, (name, body, sig, warn) in enumerate(funcs)
    )

    names_str = ", ".join(f"`{n}`" for n in target_names)
    markers_example = "\n".join(f"// ===COMET_FUNC: {n}===\n<new code for {n}>" for n in target_names)

    return f"""{diagnosis_block}Implement an optimization spanning {len(target_names)} functions that together form
one hot call chain: {names_str}. Based on the analysis above, treat them as a SINGLE
unit to optimize -- inlining one into another, eliminating a call between them,
restructuring shared state across them, etc. are all in scope, not just per-function
tweaks.

### Functions to optimize (current code, in this exact order)
{funcs_block}

### Optimization strategy
{strategy}

### CPU / SIMD context
{cpu_info}
{cpu_cache}

## Hard constraints — read BEFORE writing a single line

1. **Every one of the {len(target_names)} function names must still exist as a real,
   callable, correctly-signatured function afterward** — {names_str} are called from
   elsewhere in this codebase (outside what you can see here) with their original
   signatures (return type, parameter types, storage class/linkage). You may change
   what happens INSIDE them (including having one call, or fully absorb the body of,
   another one in this list) but you cannot delete or rename any of them, or change a
   signature that's used externally.
2. **Forbidden macros** (these expand to types/sizes — do NOT expand them yourself): {forbidden_str}
3. **Reduction precision**: NEVER add `vectorize(enable)` or `interleave(enable)` to a
   loop that accumulates into a scalar — changes FP summation order → wrong results.
4. **Single accumulator**: one scalar accumulator per output element, never split.

## Output format — REQUIRED, do not deviate

Output ALL {len(target_names)} functions, each preceded by its own marker line on its
own, exactly matching this pattern (the function name after the colon must match one
of {names_str} verbatim):

{markers_example}

No prose, no markdown fences, no explanation outside the marker lines and code —
just the {len(target_names)} marker+function blocks, back to back, in any order, each
one appearing exactly once.
"""


def _extract_original_signature(kernel_txt: str, kernel_name: str) -> str:
    """Pull the literal `<storage-class> <return-type> name(...)` header off
    an extracted function body, for telling rewrite_source's LLM the actual
    signature to preserve -- rather than assuming "static void" (true for
    PolyBench-style kernels, wrong for arbitrary hotspot.py targets like
    SPEC mcf_r's primal_iminus, which is `node_t *primal_iminus(...)` with
    real extern linkage declared in a header; the LLM adding a `static` that
    isn't there is a real linkage conflict at link time, not a style nit).
    Handles the `#ifdef _PROTO_ <ansi> #else <k&r> #endif` dual-declaration
    pattern common in this SPEC-era C code by preferring the _PROTO_ (ANSI
    prototype) branch. extract_kernel_function's start-of-body match already
    lands *inside* that guard (right after the `#ifdef _PROTO_` line, which
    is why searching for that literal marker in `kernel_txt` fails and must
    not gate this at all) -- so this looks for a bare `#else`/`#endif`
    appearing before the function's real opening `{` as the dual-declaration
    signal instead. Falls back to "" (caller keeps its generic wording) if
    nothing recognizable is found.
    """
    text = kernel_txt
    real_brace_idx = text.find("{")
    else_idx = text.find("#else")
    endif_idx = text.find("#endif")
    sig_end = else_idx if else_idx != -1 else endif_idx
    if sig_end != -1 and (real_brace_idx == -1 or sig_end < real_brace_idx):
        text = text[:sig_end]
    brace_idx = text.find("{")
    header = text[:brace_idx] if brace_idx != -1 else text
    header = header.strip()
    # Sanity check: must actually mention the function name, else this
    # extraction went wrong on unusual formatting -- don't hand the LLM
    # garbage as if it were authoritative.
    if kernel_name not in header or len(header) > 400:
        return ""
    return header


def _build_rewrite_impl_prompt(kernel_name: str, ev: dict,
                               history: "OptimizationHistory",
                               base_src_text: str,
                               strategy: str,
                               analysis_diagnosis: str) -> str:
    """
    Build the implementation prompt for the rewrite_source code-generation LLM.
    Receives the analysis diagnosis and produces the kernel code.
    """
    from src.remarks import format_rich_remarks_for_source_prompt as _fmt_rich

    # Extract just the kernel function from the resolved source
    try:
        kernel_txt, _, _ = extract_kernel_function(base_src_text, kernel_name)
        if not kernel_txt:
            kernel_txt = base_src_text
    except Exception:
        kernel_txt = base_src_text

    original_signature = _extract_original_signature(kernel_txt, kernel_name)

    pattern_warnings = _static_pattern_warnings(kernel_txt)

    forbidden_macros = ev.get("forbidden_macros", [])
    forbidden_str = ", ".join(f"`{m}`" for m in forbidden_macros[:30]) if forbidden_macros else "none"

    # Rich remarks summary (brief) -- only meaningful if `kernel_name` here
    # is the same function evidence collection ran against; src/hotspot.py
    # can pick a deeper target (e.g. mcf_r's price_out_impl, not
    # kernel_mcf_r), and these remarks describe the entry function's O3
    # pass behavior, not the actual target's -- see the matching comment
    # in _build_rewrite_analysis_prompt for why showing them anyway would
    # mislead the LLM into treating unrelated evidence as if it were about
    # the function it's writing.
    rich_remarks = ev.get("rich_remarks", {})
    remarks_str = ""
    if kernel_name == ev.get("kernel_name") and rich_remarks:
        try:
            remarks_str = _fmt_rich(rich_remarks, max_missed=6)
        except Exception:
            pass
    elif kernel_name != ev.get("kernel_name"):
        remarks_str = (f"  (not available -- collected for entry function "
                       f"{ev.get('kernel_name')!r}, not for the actual target {kernel_name!r})")

    # CPU / SIMD context
    cpu_info = ev.get("cpu_info", "")
    cpu_cache = ev.get("cpu_cache", "")

    # Failed rewrite history
    failed_rewrites = [s for s in history.steps
                       if s.action == "rewrite_source" and (s.error or s.speedup < 0.99)]
    prev_block = ""
    if failed_rewrites:
        items = [f"  - {s.strategy}: {'error' if s.error else f'{s.speedup:.3f}x regressed'}"
                 for s in failed_rewrites[-3:]]
        prev_block = ("### Previous failed rewrites — do NOT repeat the same strategy,\n"
                      "### but a DIFFERENT transformation direction is encouraged.\n"
                      + "\n".join(items) + "\n\n")

    diagnosis_block = ""
    if analysis_diagnosis:
        diagnosis_block = f"""### Expert performance analysis (read this carefully before writing)
{analysis_diagnosis}

"""

    return f"""{prev_block}{diagnosis_block}Implement an optimized C kernel for `{kernel_name}` based on the analysis above.

### Kernel to optimize
```c
{kernel_txt}
```
{pattern_warnings}
### Optimization strategy
{strategy}

### CPU / SIMD context
{cpu_info}
{cpu_cache}

### LLVM missed-optimization hints
{remarks_str or "  (none)"}

## Hard constraints — read BEFORE writing a single line

1. **Function signature**: Keep EXACTLY the same as original — same name `{kernel_name}`,
   same parameter names and types, same return type, same storage class/linkage
   (do NOT add or remove `static`/`extern` — this function's linkage is fixed by
   declarations elsewhere in the codebase; guessing wrong is a link error, not a style choice),
   same PolyBench macro forms (e.g. `DATA_TYPE POLYBENCH_2D(A,N,N,n,n)` NOT `double A[n][n]`).
   {"The ORIGINAL signature, verbatim, is: `" + original_signature + "` — reuse this exact opening." if original_signature else ""}
   NEVER add `__restrict__` or `restrict` after any `POLYBENCH_2D(...)` or `POLYBENCH_1D(...)` macro
   — the macro already expands to a multi-token type expression; appending qualifiers causes a syntax error.
2. **Forbidden macros** (these expand to types/sizes — do NOT expand them yourself): {forbidden_str}
3. **Reduction precision**: NEVER add `vectorize(enable)` or `interleave(enable)` to any loop
   that accumulates into a scalar (`acc -= A[k]*B[k]`, `sum += ...`).
   These pragmas force SIMD lane-interleaving that changes FP summation order → wrong results.
   Safe uses: non-reduction loops (initialization, copy, element-wise scale).
4. **Reduction dimension**: NEVER tile the reduction (inner-most accumulation) loop — it changes
   FP summation order. Only tile output dimensions.
5. **Matrix-multiply loop ordering**: For C[i][j] += A[i][k]*B[k][j] style (gemm/2mm/3mm/syrk/syr2k/symm),
   the cache-friendly tile order is i→k→j (outer→middle→inner). Blocking only j without blocking k
   loses B-matrix reuse and hurts performance. Use i-k-j tile nesting or k-j-i with scalar accumulator.
6. **In-place sequential algorithms** (Cholesky, LU, Gauss-Seidel): outer loop order is
   semantically fixed — each i depends on all prior rows 0..i-1. Never reorder.
6. **Single accumulator**: Use one scalar accumulator per output element. Never split into acc0+acc1.
7. **PolyBench macros**: Use `_PB_N`, `SQRT_FUN`, `EXP_FUN`, `SCALAR_VAL` — never expand them.

Output ONLY the C kernel function. No markdown fences, no explanation.
Start immediately with {"`" + original_signature + "`" if original_signature else "`static void " + kernel_name + "(` or `void " + kernel_name + "(`"}.
"""


# Old mode names kept as accepted aliases so every existing call site
# (many hardcode mode="polybench_dump"/"stdout_compare") keeps working
# unchanged after the switch to the generic, marker-free correctness tiers
# in src/correctness.py. See docs/GENERIC_HARNESS_DESIGN.md.
_MODE_ALIASES = {
    "polybench_dump": "numeric",
    "stdout_compare": "numeric",
    "numeric": "numeric",
    "hash": "hash",
    "exit_only": "exit_only",
}


def _extra_link_sources(utils: "Path | None") -> list:
    """Extra .c files a multi-file kernel needs linked (e.g. SPEC mcf_r's
    algorithm split across implicit.c/psimplex.c/... unity-built into
    utils/polybench.c by the shim generator, or TSVC's common.c/dummy.c).
    Purely a "does this kernel need more than one translation unit" build
    concern -- unrelated to whether the kernel uses any PolyBench macros."""
    if not utils:
        return []
    pc = utils / "polybench.c"
    return [str(pc)] if pc.exists() else []


def _detect_polybench_mode(clang: str, src_path: str, utils: Path,
                           source_dir: Path, tmpdir: Path) -> str:
    """
    自动检测 benchmark 的正确性验证模式（返回值见 src/correctness.py 的
    三档：numeric > hash > exit_only）。不再要求目标程序 #include
    <polybench.h> 或调用任何 DUMP/instrument 宏——直接编译一个普通二进制，
    跑起来看它的原生输出里有没有能提取的数值、是否确定性即可。

    仍然带上 -DPOLYBENCH_DUMP_ARRAYS：对 SPEC/TSVC/CBench 新版 wrapper 这
    个宏根本没被引用，纯粹无操作；但对真正的 PolyBench-C kernel（2mm/gemm
    等，自带 polybench.h 那套数组宏），它是 polybench_prevent_dce() 内部
    `if (argc > 42 && ...)` 死代码消除守卫的开关——不定义它，print_array()
    永远不会真的执行，correctness check 会悄悄退化成"比较两个空输出"，
    等于没检查。这正是 mcf_r 那次系统性 bug 的同一种坑，只是换了个 kernel
    类型触发。
    """
    include_dirs = [utils, source_dir] if utils else [source_dir]
    extra_src = _extra_link_sources(utils)
    test_bin = tmpdir / "_detect_test"
    ok, _ = compile_c(clang, [src_path] + extra_src, include_dirs,
                      ["-DSMALL_DATASET", "-DPOLYBENCH_DUMP_ARRAYS"], test_bin)
    if not ok:
        return "exit_only"
    return detect_correctness_mode(str(test_bin), timeout=20)


def _correctness_check(clang: str, src_path: str, ref_src: str,
                       dataset_flag: str, tmpdir: Path, tag: str,
                       utils: Path, source_dir: Path,
                       epsilon: float, timeout: int = 60,
                       mode: str = "numeric",
                       extra_flags: "list | None" = None) -> Tuple[bool, str]:
    """
    通用正确性检查——编译 ref/候选 两个二进制，跑起来交给
    src/correctness.py 按 mode 对比（numeric 数值容差 / hash 精确哈希 /
    exit_only 仅退出码）。不要求目标程序包含任何特定头文件或宏；`utils`
    仍然是多文件 kernel 需要一起链接的额外 .c 来源（跟是否用 PolyBench
    无关，纯粹是"这个 kernel 有几个翻译单元"的构建问题）。

    返回 (passed, error_msg)。
    """
    mode = _MODE_ALIASES.get(mode, "numeric")
    include_dirs = [utils, source_dir] if utils else [source_dir]
    extra_src = _extra_link_sources(utils)
    # See _detect_polybench_mode's docstring: no-op for SPEC/TSVC/CBench's
    # macro-free wrappers, required to unlock real PolyBench-C kernels'
    # print_array() (gated behind this by polybench_prevent_dce otherwise).
    _defines = [f"-D{dataset_flag}", "-DPOLYBENCH_DUMP_ARRAYS"]

    ref_bin = tmpdir / f"{tag}_ref_{dataset_flag}"
    ok, err = compile_c(clang, [ref_src] + extra_src, include_dirs,
                        _defines, ref_bin)
    if not ok:
        # dataset_flag 不存在时降级到 MINI_DATASET
        if dataset_flag == "STANDARD_DATASET":
            return _correctness_check(
                clang, src_path, ref_src, "MINI_DATASET",
                tmpdir, tag + "_mini", utils, source_dir,
                epsilon, timeout=30, mode=mode, extra_flags=extra_flags)
        return False, f"ref 编译失败 ({dataset_flag}): {clean_clang_diagnostics(err, max_diagnostics=3)}"

    opt_bin = tmpdir / f"{tag}_opt_{dataset_flag}"
    ok, cerr = compile_c(clang, [src_path] + extra_src, include_dirs,
                         _defines, opt_bin, extra_flags=extra_flags)
    if not ok:
        return False, f"优化版编译失败 ({dataset_flag}): {clean_clang_diagnostics(cerr, max_diagnostics=3)}"

    passed, msg = check_correctness(str(ref_bin), str(opt_bin), mode,
                                    epsilon=epsilon, timeout=timeout)
    if not passed and "timed out" in msg and dataset_flag == "STANDARD_DATASET":
        # STANDARD 超时 → 降级到 MINI
        return _correctness_check(
            clang, src_path, ref_src, "MINI_DATASET",
            tmpdir, tag + "_mini", utils, source_dir,
            epsilon, timeout=30, mode=mode, extra_flags=extra_flags)
    if not passed and mode == "numeric" and "empty" in msg:
        # 没有可提取的数值输出 → 降级到 exit_only（跟旧版行为一致）
        return _correctness_check(
            clang, src_path, ref_src, dataset_flag,
            tmpdir, tag, utils, source_dir, epsilon, timeout,
            mode="exit_only", extra_flags=extra_flags)
    if not passed and mode == "numeric":
        return False, f"[{dataset_flag}] {msg}\n{_diagnose_precision_error(msg)}"
    return passed, msg


def _save_snapshot(src_text: str, snapshot_dir: Path, step_num: int,
                   action: str, status: str = "ok") -> Path:
    """
    将源码快照保存到 snapshot_dir/step_{N}_{action}_{status}.c。
    始终保存，即使失败版本也要留记录。
    """
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    fname = f"step_{step_num:02d}_{action}_{status}.c"
    fpath = snapshot_dir / fname
    fpath.write_text(src_text)
    return fpath


def _eval_utils_rewrite(clang: str, driver_path: str, orig_utils_dir: Path,
                        rewritten_polybench_c: str, tmpdir: Path, tag: str,
                        source_dir: Path, runs: int, pin_cpu,
                        epsilon: float, correctness_mode: str) -> dict:
    """
    Evaluate a rewrite_source candidate that targets a function living in
    utils/polybench.c (see src/hotspot.py) rather than the driver file.

    _eval_build_and_time can't be reused here: it always builds the
    reference and the candidate against the *same* utils Path, which is
    correct when only the driver file changes (the normal case) but wrong
    here, where the driver is identical on both sides and it's the utils
    content that differs -- the reference build must use the ORIGINAL
    utils/polybench.c and the candidate build a modified copy in a shadow
    directory, never touching the original.

    Deliberately simpler than _eval_build_and_time's full pipeline: no
    MINI_DATASET timeout downgrade, no perf_stats collection, no
    precision-fix/compile-fix LLM retries (those stay exclusive to the
    driver-file rewrite path for now) -- a real, working first cut, not
    full feature parity.

    Returns {"ok", "speedup", "error"}.
    """
    orig_pc = orig_utils_dir / "polybench.c"
    shadow_dir = tmpdir / f"{tag}_utils_shadow"
    shadow_dir.mkdir(exist_ok=True)
    (shadow_dir / "polybench.c").write_text(rewritten_polybench_c)
    for f in orig_utils_dir.iterdir():
        if f.name != "polybench.c" and f.is_file():
            shutil.copy(f, shadow_dir / f.name)

    mode = _MODE_ALIASES.get(correctness_mode, "numeric")
    for ds, eps, tmo in (("SMALL_DATASET", epsilon, 45),
                         ("STANDARD_DATASET", epsilon * 2.0, 120)):
        ref_bin = tmpdir / f"{tag}_ref_{ds}"
        ok, err = compile_c(clang, [driver_path, str(orig_pc)],
                            [orig_utils_dir, source_dir],
                            [f"-D{ds}", "-DPOLYBENCH_DUMP_ARRAYS"], ref_bin)
        if not ok:
            return {"ok": False, "error": f"ref 编译失败 ({ds}): {clean_clang_diagnostics(err, max_diagnostics=3)}"}

        opt_bin = tmpdir / f"{tag}_opt_{ds}"
        ok2, err2 = compile_c(clang, [driver_path, str(shadow_dir / "polybench.c")],
                              [shadow_dir, source_dir],
                              [f"-D{ds}", "-DPOLYBENCH_DUMP_ARRAYS"], opt_bin)
        if not ok2:
            return {"ok": False, "error": f"候选编译失败 ({ds}): {clean_clang_diagnostics(err2, max_diagnostics=3)}"}

        passed, msg = check_correctness(str(ref_bin), str(opt_bin), mode,
                                        epsilon=eps, timeout=tmo)
        if not passed:
            return {"ok": False, "error": f"[{ds}] {msg}"}

    ref_time_bin = tmpdir / f"{tag}_ref_time"
    ok, _ = compile_c(clang, [driver_path, str(orig_pc)],
                      [orig_utils_dir, source_dir], ["-DLARGE_DATASET"], ref_time_bin)
    ref_t = ts_run_timing(str(ref_time_bin), runs=runs, pin_cpu=pin_cpu) if ok else -1.0

    opt_time_bin = tmpdir / f"{tag}_opt_time"
    ok2, err2 = compile_c(clang, [driver_path, str(shadow_dir / "polybench.c")],
                          [shadow_dir, source_dir], ["-DLARGE_DATASET"], opt_time_bin)
    if not ok2:
        return {"ok": False, "error": f"计时编译失败: {clean_clang_diagnostics(err2, max_diagnostics=3)}"}
    opt_t = ts_run_timing(str(opt_time_bin), runs=runs, pin_cpu=pin_cpu)

    if ref_t <= 0 or opt_t <= 0:
        return {"ok": False, "error": "计时失败（ref 或候选运行未产生有效耗时）"}
    return {"ok": True, "speedup": ref_t / opt_t, "error": ""}


def _eval_build_and_time(clang: str, src_path: str, ref_src: str,
                         extra_flags: list, tmpdir: Path, tag: str,
                         utils: Path, source_dir: Path,
                         runs: int, pin_cpu,
                         epsilon: float = 1e-4,
                         snapshot_dir: "Path | None" = None,
                         step_num: int = 0,
                         action: str = "eval",
                         correctness_mode: str = "auto") -> dict:
    """
    编译 src_path，双重正确性验证，然后 LARGE_DATASET 计时 + perf stat。

    correctness_mode:
      "auto"            — 自动检测（优先 polybench_dump，fallback stdout_compare）
      "polybench_dump"  — PolyBench SMALL + STANDARD 数值比对（最严格）
      "stdout_compare"  — 比较 stdout 输出
      "exit_only"       — 只检查不 crash

    正确性流程：
      1. SMALL_DATASET（或等价）：tolerance=epsilon
      2. STANDARD_DATASET（或等价，超时自动降级为 MINI）：tolerance=epsilon*2

    返回 {ok, speedup, time_ms, perf_stats, snapshot_path, error}
    """
    polybench_c  = utils / "polybench.c" if utils else None
    include_dirs = [utils, source_dir] if utils else [source_dir]

    # 读取被测源码（用于快照）
    try:
        with open(src_path) as f:
            src_text = f.read()
    except Exception:
        src_text = ""

    # 自动检测正确性模式（走 src/correctness.py 的通用三档检测，跟
    # _detect_polybench_mode 用同一套逻辑——不要求目标程序有任何特定宏）。
    # 实际主流程里 ev["correctness_mode"] 在 evidence collection 阶段就已经
    # 定好了，这里的 "auto" 只在极少数直接调用 _eval_build_and_time 且没有
    # 预先探测过的场合触发；保留是为了不留一条仍然依赖旧 DUMP_ARRAYS 宏的
    # 探测分支在代码里（那条分支本身就是 mcf_r/TSVC 那次静默 1.0x 的根因）。
    if correctness_mode == "auto":
        extra_src = [str(polybench_c)] if polybench_c and polybench_c.exists() else []
        _test = tmpdir / f"{tag}_autodetect"
        _ok, _ = compile_c(clang, [ref_src] + extra_src, include_dirs, ["-DSMALL_DATASET"], _test)
        correctness_mode = detect_correctness_mode(str(_test), timeout=20) if _ok else "exit_only"

    # 是否有多文件 utils（framework 无关：单纯是"这个 kernel 有没有额外翻译单元
    # /数据集大小宏"，跟 correctness_mode 是 numeric/hash/exit_only 完全独立——
    # 之前用 `correctness_mode == "polybench_dump"` 判断，把 mode 从
    # "polybench_dump" 改名成 "numeric" 之后这个比较永远为假，会让真正的
    # PolyBench kernel 在计时阶段悄悄丢掉 -DLARGE_DATASET，量出来的时间会是
    # 错的数据集规模。
    _has_utils = bool(polybench_c and polybench_c.exists())
    _extra_src = [str(polybench_c)] if _has_utils else []

    # ── 第一层正确性：SMALL 规模 ──────────────────────────────────────────────
    ds1 = "SMALL_DATASET" if _has_utils else "n/a"
    ok1, err1 = _correctness_check(
        clang, src_path, ref_src, ds1,
        tmpdir, tag + "_c1", utils, source_dir,
        epsilon, timeout=45, mode=correctness_mode, extra_flags=extra_flags)
    if not ok1:
        if snapshot_dir:
            _save_snapshot(src_text, snapshot_dir, step_num, action, "FAIL_lvl1")
        return {"ok": False, "speedup": 1.0, "error": err1, "perf_stats": {},
                "snapshot_path": ""}

    # ── 第二层正确性：STANDARD 规模（超时自动降级为 MINI）────────────────────
    ds2 = "STANDARD_DATASET" if _has_utils else "n/a"
    ok2, err2 = _correctness_check(
        clang, src_path, ref_src, ds2,
        tmpdir, tag + "_c2", utils, source_dir,
        epsilon * 2.0, timeout=120, mode=correctness_mode, extra_flags=extra_flags)
    if not ok2:
        if snapshot_dir:
            _save_snapshot(src_text, snapshot_dir, step_num, action, "FAIL_lvl2")
        return {"ok": False, "speedup": 1.0,
                "error": f"第二层验证失败: {err2}", "perf_stats": {},
                "snapshot_path": ""}

    # ── 快照：验证通过的版本 ──────────────────────────────────────────────────
    snap_path = ""
    if snapshot_dir:
        snap_file = _save_snapshot(src_text, snapshot_dir, step_num, action, "ok")
        snap_path = str(snap_file)

    # ── 计时：ref + opt，LARGE_DATASET ────────────────────────────────────────
    _time_defines = ["-DLARGE_DATASET"] if _has_utils else []
    ref_time_bin = tmpdir / f"{tag}_ref_time"
    _ok, _ = compile_c(clang, [ref_src] + _extra_src, include_dirs,
                       _time_defines, ref_time_bin, extra_flags=extra_flags)
    ref_t = ts_run_timing(str(ref_time_bin), runs=runs, pin_cpu=pin_cpu) if _ok else -1.0

    opt_time_bin = tmpdir / f"{tag}_opt_time"
    _ok2, _err2 = compile_c(clang, [src_path] + _extra_src, include_dirs,
                            _time_defines, opt_time_bin, extra_flags=extra_flags)
    if not _ok2:
        return {"ok": False, "speedup": 1.0,
                "error": f"计时编译失败: {_err2[:200]}",
                "perf_stats": {}, "snapshot_path": snap_path}

    opt_t = ts_run_timing(str(opt_time_bin), runs=runs, pin_cpu=pin_cpu)
    if opt_t <= 0:
        return {"ok": False, "speedup": 1.0,
                "error": "计时程序无输出", "perf_stats": {}, "snapshot_path": snap_path}

    speedup = (ref_t / opt_t) if ref_t > 0 else 1.0

    # ── perf stat（优化版，1次，收集硬件计数器）──────────────────────────────
    perf_stats: dict = {}
    try:
        from src.perf_analysis import collect_perf_stats
        perf_stats = collect_perf_stats(str(opt_time_bin), runs=1, pin_cpu=pin_cpu)
    except Exception:
        pass

    return {
        "ok":           True,
        "speedup":      speedup,
        "time_ms":      opt_t,
        "ref_time_ms":  ref_t,
        "error":        "",
        "perf_stats":   perf_stats,
        "snapshot_path": snap_path,
        "correctness_mode": correctness_mode,
    }



# ── Agent step ──────────────────────────────────────────────────────────────

def run_agent_step(src_original: str, config, llm: LLMClient,
                   ev: dict, runs: int, pin_cpu, epsilon: float, max_tokens: int,
                   history: "OptimizationHistory",
                   current_best_source: "str | None",
                   current_best_flags: list,
                   step_num: int, max_steps: int,
                   baseline_time: float,
                   snapshot_dir: "Path | None" = None,
                   forced_action: "str | None" = None) -> dict:
    """
    Agent 单步：LLM 决定执行什么 action，系统执行并验证。

    返回：{action, reasoning, speedup, strategy, error, flags, source,
            perf_stats, snapshot_path}
    source 只在有更好源码重写时才设置。
    perf_stats 记录本步骤的硬件计数器（用于 history 趋势展示）。
    """
    clang       = config.compiler.clang_path
    name        = Path(src_original).stem
    kernel_name = ev["kernel_name"]
    utils       = ev["utils"]
    source_dir  = ev["source_dir"]
    polybench_c = (utils / "polybench.c") if utils else None

    cpu_info  = get_cpu_info()
    cpu_cache = ts_get_cache_info()

    prompt = _build_agent_prompt(
        kernel_name, ev, cpu_info, cpu_cache,
        history, current_best_source, current_best_flags,
        step_num, max_steps,
        forced_action=forced_action,
    )

    print(f"  Querying LLM (step {step_num})...")
    resp = llm.call([
        {"role": "system",
         "content": "You are a compiler optimization agent. Output strict JSON only."},
        {"role": "user", "content": prompt},
    ], max_tokens=max_tokens, temperature=0.15)

    if not resp:
        return {"action": "error", "speedup": 1.0, "error": "no LLM response",
                "reasoning": "", "strategy": "", "flags": [], "source": None}

    try:
        plan = json.loads(strip_json_fences(resp))
    except Exception as e:
        return {"action": "error", "speedup": 1.0, "error": f"JSON parse: {e}",
                "reasoning": "", "strategy": "", "flags": [], "source": None}

    action    = plan.get("action", "done")
    reasoning = plan.get("reasoning", "")

    # ── 强制 action 覆盖：若 forced_action 已设置，LLM 不得选择其他 action ──────
    if forced_action and action != forced_action:
        print(f"  [forced] LLM chose '{action}', overriding to '{forced_action}'")
        action = forced_action
        # 若被强制为 rewrite_source 但 LLM 没给 strategy，补一个默认 strategy
        if forced_action == "rewrite_source" and not plan.get("strategy"):
            plan["strategy"] = (
                "根据 perf 瓶颈和 missed remarks 做结构变换："
                "优先考虑 loop tiling / 循环交换 / scalar accumulator，消除缓存缺失或解锁向量化"
            )
        # 若被强制为 try_flags 但 LLM 没给 flags，交给 auto-supplement 处理（flag_specs=[]）
        if forced_action == "try_flags" and not plan.get("flags"):
            plan["flags"] = []

    improvement_analysis = plan.get("improvement_analysis", "")
    print(f"  Action: {action}")
    print(f"  Reasoning: {reasoning[:200]}")
    if improvement_analysis:
        print(f"  ImprovementAnalysis: {improvement_analysis[:200]}")

    if action == "done":
        return {"action": "done", "speedup": history.best_speedup,
                "strategy": plan.get("reason", "done"), "error": "",
                "reasoning": reasoning,
                "improvement_analysis": improvement_analysis,
                "flags": [], "source": None}

    # Resolve "base" -- which source to start from
    base_choice = plan.get("base", "original")
    if base_choice == "current_best" and current_best_source:
        base_src_text = current_best_source
    else:
        with open(src_original) as f:
            base_src_text = f.read()

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        # ── try_flags ─────────────────────────────────────────────────────────
        def _do_try_flags() -> dict:
            flag_specs = list(plan.get("flags", []))

            # ── Auto-supplement: add targeted_passes params not already in LLM suggestions ──
            # Selection criteria (all three must hold):
            #   1. Pass actually ran in the O3 pipeline (in ev["targeted_passes"])
            #   2. Pass has missed remarks (optimization opportunities exist)
            #   3. Parameter exists in this LLVM binary (in ev["discovered_opts"] OR in
            #      _DEFAULT_FLAG_CANDIDATES whose entries are verified via opt --help-hidden)
            # Candidate values: from _DEFAULT_FLAG_CANDIDATES, always toward MORE aggressive
            # optimization (see comments there for per-parameter justification).
            _discovered = ev.get("discovered_opts", {})
            _already = {spec.get("flag", "").lstrip("-") for spec in flag_specs}
            for _tp in ev.get("targeted_passes", []):
                if _tp.get("missed_count", 0) == 0:
                    continue  # skip passes with no missed remarks
                pname = _tp["pass_name"]
                for _param in _tp.get("params", []):
                    _key = _param.lstrip("-")
                    if _key in _already:
                        continue
                    # Verify: parameter must exist in discovered_opts (opt --help-hidden validated)
                    _opt_entry = next(
                        (o for o in _discovered.get(pname, []) if _key in o.get("flag", "")),
                        None
                    )
                    if _opt_entry is None:
                        continue
                    # Infer candidates from compiler-reported type + description
                    _cands = _infer_candidates_from_desc(
                        _param,
                        _opt_entry.get("type", "uint"),
                        _opt_entry.get("desc", ""),
                    )
                    if _cands:
                        flag_specs.append({
                            "flag": _param,
                            "candidates": _cands,
                            "rationale": (
                                f"auto: {pname} ran in O3 + has {_tp['missed_count']} missed remarks; "
                                f"type={_opt_entry.get('type','?')}, "
                                f"desc='{_opt_entry.get('desc','')[:60]}'"
                            ),
                        })
                        _already.add(_key)

            if not flag_specs:
                return {"action": action, "speedup": 1.0, "error": "no flags specified",
                        "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                        "strategy": "", "flags": [], "source": None}

            # Write base source to a temp file
            base_file = tmpdir / f"{name}_base.c"
            base_file.write_text(base_src_text)

            best_t       = baseline_time
            best_flags   = list(current_best_flags)
            all_results  = []

            # Phase A: independent per-flag search
            # Fast single-run screening: with the flag budget raised to 10-20 (see the
            # try_flags prompt section), timing every candidate with the full `runs`
            # count would make each step much slower for no benefit -- Phase A only
            # needs to rank candidates well enough to pick per-flag winners for Phase B.
            # The eventual winner gets a full-`runs` re-time below before its speedup
            # is reported or gates the correctness check, so precision isn't lost where
            # it actually matters.
            _screen_runs = 1
            # Also record per-flag compile time to dynamically exclude slow-to-compile flags
            # from Phase B (flags whose best candidate took >_JOINT_COMPILE_WARN_S to compile
            # are likely to cause exponential vectorizer search when combined with others).
            _JOINT_COMPILE_WARN_S = 30.0  # flags slower than this to compile stay Phase-A only
            best_per_flag: Dict[str, tuple] = {}
            flag_compile_time: Dict[str, float] = {}  # flag -> fastest compile time (seconds)
            for pidx, spec in enumerate(flag_specs):
                flag  = spec.get("flag", "")
                cands = spec.get("candidates", [])
                if not flag or not cands:
                    continue
                flag_best_t = baseline_time
                flag_best_v = None
                flag_min_compile_s = float("inf")
                for val in cands:
                    import time as _time
                    mflags = ["-mllvm", f"{flag}={val}"]
                    cand   = tmpdir / f"tf_{pidx}_{val}"
                    _t0 = _time.monotonic()
                    ok, _ = compile_binary(clang, str(base_file), polybench_c,
                                           utils, source_dir, cand,
                                           extra_flags=mflags)
                    _compile_s = _time.monotonic() - _t0
                    if not ok:
                        continue
                    flag_min_compile_s = min(flag_min_compile_s, _compile_s)
                    t = tp_run_timing(str(cand), runs=_screen_runs, pin_cpu=pin_cpu)
                    if t <= 0:
                        continue
                    sp = baseline_time / t
                    all_results.append({"flag": flag, "val": val, "speedup": sp})
                    mark = " <--" if t < flag_best_t else ""
                    print(f"    {flag}={val} -> {t:.1f} ms ({sp:.3f}x){mark}")
                    if t < flag_best_t:
                        flag_best_t = t
                        flag_best_v = val
                best_per_flag[flag] = (flag_best_t, flag_best_v)
                flag_compile_time[flag] = flag_min_compile_s
                if flag_best_t < best_t:
                    best_t = flag_best_t
                    best_flags = ["-mllvm", f"{flag}={flag_best_v}"]

            # Phase B: joint combination of best flags (top-2/3)
            # Dynamically exclude flags whose Phase-A compile took >_JOINT_COMPILE_WARN_S:
            # these likely trigger exponential optimizer search when combined with other flags.
            winners = sorted(
                [(fn, v, t) for fn, (t, v) in best_per_flag.items()
                 if v is not None and t < baseline_time
                 and flag_compile_time.get(fn, 0) <= _JOINT_COMPILE_WARN_S],
                key=lambda x: x[2]
            )
            if len(winners) >= 2:
                # Try top-2, then top-3 (only if top-2 succeeds quickly)
                for n_joint in [2, 3]:
                    if n_joint > len(winners):
                        break
                    joint_flags = []
                    for fn, v, _ in winners[:n_joint]:
                        joint_flags += ["-mllvm", f"{fn}={v}"]
                    cand = tmpdir / f"tf_joint_{n_joint}"
                    ok, err = compile_binary(clang, str(base_file), polybench_c,
                                            utils, source_dir, cand,
                                            extra_flags=joint_flags, timeout=150)
                    if not ok:
                        print(f"    [joint-{n_joint}] compile failed/timeout, skipping larger combos")
                        break
                    t = tp_run_timing(str(cand), runs=runs, pin_cpu=pin_cpu)
                    if t > 0:
                        sp = baseline_time / t
                        print(f"    [joint-{n_joint}] {' '.join(joint_flags)} -> {sp:.3f}x")
                        if t < best_t:
                            best_t     = t
                            best_flags = joint_flags

            # ── 排名靠前的候选重新验证：交替测量，而不是再跑几次同一个二进制 ──
            # Phase A/B 用 _screen_runs=1 快速筛选，单次测量的噪声足以把一个
            # 实际不到 1.7x 的 flag 误判成 2.76x 的"新最优"（doitgen 步骤6 真实
            # 出现过）。这里不是简单把赢家多测 N 次求平均——那治不了"搜索过程
            # 越往后系统负载越高"这种跨候选的系统性漂移；而是复用
            # confirm_result 同款的 baseline/candidate 交替测量 + 配对中位数
            # 手法，与一个刚编译出来、不带任何候选 flag 的纯净 base_file 直接
            # 交替对比，两边共享同一段测量窗口，能互相抵消漂移。
            if best_flags and best_flags != list(current_best_flags) and _screen_runs < runs:
                _screen_t = best_t  # Phase A/B 单次筛选给出的时间，仅用于下面打印对比
                _verify_bin = tmpdir / "tf_verify_best"
                ok, _ = compile_binary(clang, str(base_file), polybench_c,
                                       utils, source_dir, _verify_bin,
                                       extra_flags=best_flags)
                if ok:
                    _plain_bin = tmpdir / "tf_verify_plain_base"
                    ok_plain, _ = compile_binary(clang, str(base_file), polybench_c,
                                                 utils, source_dir, _plain_bin)
                    # confirm_result_external, NOT confirm_result: best_t below gets
                    # compared against baseline_time, which was measured via
                    # run_timing()'s external wall-clock -- confirm_result's
                    # stdout-self-report parsing is a different methodology and
                    # produces nonsense when mixed with it (see its docstring).
                    _confirm = (confirm_result_external(str(_plain_bin), str(_verify_bin), runs, pin_cpu)
                               if ok_plain else {"ok": False})
                    if _confirm.get("ok"):
                        best_t = _confirm["best_median_ms"]
                        _screen_sp = baseline_time / _screen_t if _screen_t > 0 else 0.0
                        print(f"    [候选交替验证] {' '.join(best_flags)}: "
                              f"单次筛选 {_screen_sp:.3f}x → "
                              f"交替确认 {_confirm['confirmed_speedup']:.3f}x "
                              f"(IQR [{_confirm['speedup_iqr'][0]:.3f}, {_confirm['speedup_iqr'][1]:.3f}])")
                    else:
                        _t = tp_run_timing(str(_verify_bin), runs=runs, pin_cpu=pin_cpu)
                        best_t = _t if _t > 0 else baseline_time
                else:
                    best_t = baseline_time
                if best_t >= baseline_time:
                    best_flags = list(current_best_flags)

            # ── perf stat on best result ─────────────────────────────────────
            best_perf: dict = {}
            if best_t < baseline_time:
                # compile best binary for perf measurement
                _perf_bin = tmpdir / "tf_perf_best"
                ok, _ = compile_binary(clang, str(base_file), polybench_c,
                                       utils, source_dir, _perf_bin,
                                       extra_flags=best_flags)
                if ok:
                    try:
                        from src.perf_analysis import collect_perf_stats
                        best_perf = collect_perf_stats(str(_perf_bin), runs=1, pin_cpu=pin_cpu)
                    except Exception:
                        pass

            sp    = baseline_time / best_t if best_t < baseline_time else 1.0
            strat = f"flags: {' '.join(best_flags)}" if best_flags else "无改善"

            # ── 正确性验证：-mllvm cost-model flags 会改变 vectorizer/unroller
            # 的实际决策（例如 SLP 向量化会重排浮点加法），并非纯粹"无副作用"的
            # 调参 —— 之前 try_flags 分支完全没有调用 _correctness_check，一个
            # 使程序算错但恰好更快的 flag 组合会被无声地当作"改进"接受。
            # 用与 try_pragma/rewrite_source 相同的两层 SMALL+STANDARD 数值验证。
            if best_flags and sp > 1.0:
                ok1, err1 = _correctness_check(
                    clang, str(base_file), str(base_file), "SMALL_DATASET",
                    tmpdir, "tf_correct1", utils, source_dir,
                    epsilon, timeout=45, mode="polybench_dump", extra_flags=best_flags)
                ok2 = True
                err2 = ""
                if ok1:
                    ok2, err2 = _correctness_check(
                        clang, str(base_file), str(base_file), "STANDARD_DATASET",
                        tmpdir, "tf_correct2", utils, source_dir,
                        epsilon * 2.0, timeout=120, mode="polybench_dump", extra_flags=best_flags)
                if not (ok1 and ok2):
                    print(f"  ⚠ try_flags 候选 {strat} 数值验证失败，拒绝该 flags 组合: "
                          f"{(err1 or err2)[:200]}")
                    return {"action": action, "speedup": 1.0, "strategy": "",
                            "error": f"flags 数值验证失败: {(err1 or err2)[:300]}",
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "flags": [], "source": None, "perf_stats": {}}

            print(f"  try_flags 最优: {sp:.3f}x  [{strat}]")
            return {"action": action, "speedup": sp, "strategy": strat, "error": "",
                    "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                    "flags": best_flags, "source": None,
                    "perf_stats": best_perf}

        # ── try_pragma ────────────────────────────────────────────────────────
        def _do_try_pragma() -> dict:
            hints      = plan.get("pragma_hints", [])
            also_flags = plan.get("also_flags", [])
            if not hints:
                return {"action": action, "speedup": 1.0, "error": "pragma_hints 为空",
                        "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                        "strategy": "", "flags": [], "source": None,
                        "perf_stats": {}}

            pragma_src = _apply_pragma_hints(base_src_text, hints)
            if pragma_src == base_src_text:
                return {"action": action, "speedup": 1.0,
                        "error": "未找到匹配的 for 循环前缀",
                        "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                        "strategy": "", "flags": [], "source": None,
                        "perf_stats": {}}

            _psrc = tmpdir / f"{name}_pragma.c"
            _psrc.write_text(pragma_src)

            extra = also_flags or current_best_flags
            res = _eval_build_and_time(
                clang, str(_psrc), src_original,
                extra_flags=extra, tmpdir=tmpdir, tag="pragma",
                utils=utils, source_dir=source_dir,
                runs=runs, pin_cpu=pin_cpu, epsilon=epsilon,
                snapshot_dir=snapshot_dir, step_num=step_num, action=action,
                correctness_mode=ev.get("correctness_mode", "auto"),
            )
            if not res["ok"]:
                return {"action": action, "speedup": 1.0, "error": res["error"][:200],
                        "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                        "strategy": "", "flags": [], "source": None,
                        "perf_stats": {}}

            sp     = res["speedup"]
            descs  = [h.get("pragma", "") for h in hints]
            strat  = "pragma: " + "; ".join(descs[:2])
            source = pragma_src if sp > 1.001 else None
            flags  = extra if sp > 1.001 else []
            snap   = res.get("snapshot_path", "")
            print(f"  try_pragma: {sp:.3f}x  [{strat}]")
            return {"action": action, "speedup": sp, "strategy": strat, "error": "",
                    "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                    "flags": flags, "source": source,
                    "perf_stats": res.get("perf_stats", {}), "snapshot_path": snap}

        # ── rewrite_source ────────────────────────────────────────────────────
        def _do_rewrite_source() -> dict:
            strategy   = plan.get("strategy", "")
            also_flags = plan.get("also_flags", [])
            # base_src_text and base_choice already resolved above (see base_choice = plan.get("base", ...))

            # ── 热点函数：只在 main() 里 collect_all_evidence 之后选一次（见
            # ev["hotspot_target"]），这里绝不重新跑 select_hotspot_target ──
            # 原因：这个函数名是调用图的结构性事实，run 期间不会变，重新选一次
            # 没有意义；真正会变的是它的正文（改写被接受后），所以正文永远在这
            # 里现读现取。之前每步都重新选且外层 agent 决策 prompt（_build_agent_prompt，
            # 决定 action + strategy 那一步）完全不知道热点重定向这回事，导致它
            # 提的 strategy 描述的是 kernel_name 的改动（比如"合并 kernel_mcf_r 的
            # printf"），而实际写代码、验证、生效的却是热点函数（primal_iminus）——
            # 两者对不上，等于用错误的意图指导实现阶段的 LLM。见与本次改动同批的
            # commit message。现在 target_name 和 in_utils 从 ev 里读缓存值，与
            # _build_agent_prompt 看到的完全一致。
            _utils_text = None
            if utils:
                _pc = Path(utils) / "polybench.c"
                if _pc.exists():
                    try:
                        _utils_text = _pc.read_text(errors="replace")
                    except OSError:
                        pass
            target_name = ev.get("hotspot_target", kernel_name)
            _hotspot_in_utils = ev.get("hotspot_in_utils", False) and target_name != kernel_name
            context_text = _utils_text if _hotspot_in_utils else base_src_text

            # ── 多函数联合重写：select_hotspot_targets 发现热点分数彼此接近、
            # 分散在多个函数时（见 src/hotspot.py），ev["hotspot_targets"] 会有
            # 不止一个名字。单独改分数最高的那一个天花板太低——mcf_r 的
            # primal_iminus/switch_arcs/markBaskets 分数差不到4%，只改一个
            # 完全动不了另外两个占大头的部分。这里的分支只影响 Phase 2 的实现
            # LLM 调用和后面的拼接逻辑；Phase 1 的瓶颈诊断仍然只对分数最高的
            # target_name 跑（诊断是给人/LLM看的文字，不是要拼回源码的代码，
            # 针对主要目标诊断已经足够指导策略）。
            target_names = ev.get("hotspot_targets") or [target_name]
            _is_multi = len(target_names) > 1

            # ── Phase 1: Analysis LLM — diagnose bottleneck before writing code ──
            print(f"  [重写分析] 运行瓶颈诊断 LLM...")
            analysis_diagnosis = analyze_rewrite_bottleneck(
                llm, target_name, ev, history,
                base_src_text=context_text,
                strategy=strategy,
                max_tokens=max_tokens,
            )
            if analysis_diagnosis:
                first_line = analysis_diagnosis.splitlines()[0][:130]
                print(f"  [分析结论] {first_line}")
            else:
                print(f"  [重写分析] 诊断失败，直接进入实现")

            # ── Phase 2: Implementation LLM — write kernel code using diagnosis ──
            print(f"  [重写实现] 根据分析生成优化代码...")
            multi_chunks = None
            if _is_multi:
                print(f"  [多函数联合] 目标 = {', '.join(target_names)}")
                impl_prompt = _build_rewrite_impl_prompt_multi(
                    target_names, ev, history,
                    base_src_text=context_text,
                    strategy=strategy,
                    analysis_diagnosis=analysis_diagnosis,
                )
                impl_system = (
                    "You are a C performance engineering expert implementing an optimization "
                    "that spans multiple functions. Read the analysis carefully, then implement "
                    "exactly what it recommends. Output ONLY the marked function blocks the "
                    "prompt asks for — no prose, no markdown, no fences."
                )
                impl_resp = llm.call(
                    [{"role": "system", "content": impl_system},
                     {"role": "user",   "content": impl_prompt}],
                    max_tokens=max_tokens,
                    timeout=150,
                )
                raw_multi = _strip_fences(impl_resp) if impl_resp else ""
                multi_chunks = _parse_multi_func_response(raw_multi, target_names) if raw_multi else None
                if multi_chunks is None:
                    return {"action": action, "speedup": 1.0,
                            "error": (f"多函数重写响应解析失败：期望 {target_names} 各恰好一次的 "
                                     f"// ===COMET_FUNC: name=== 标记，实际未能匹配"),
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}
                raw_kernel = None  # not used on the multi path; multi_chunks carries the result
            else:
                impl_prompt = _build_rewrite_impl_prompt(
                    target_name, ev, history,
                    base_src_text=context_text,
                    strategy=strategy,
                    analysis_diagnosis=analysis_diagnosis,
                )
                impl_system = (
                    "You are a C performance engineering expert implementing an optimization. "
                    "Read the analysis carefully, then implement exactly what it recommends. "
                    "Output ONLY the C kernel function — no prose, no markdown, no fences."
                )
                impl_resp = llm.call(
                    [{"role": "system", "content": impl_system},
                     {"role": "user",   "content": impl_prompt}],
                    max_tokens=max_tokens,
                    timeout=120,
                )
                raw_kernel = _strip_fences(impl_resp) if impl_resp else ""

                if not raw_kernel:
                    return {"action": action, "speedup": 1.0, "error": "实现 LLM 未返回 kernel_code",
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}

            # ── 热点在 utils/polybench.c 里：driver 文件本身不变，ref 用原始
            # utils，候选用改写后的影子 utils（见 _eval_utils_rewrite）。通过就
            # 直接写回本次 run 的私有 utils 路径持久化，后续步骤自动生效——
            # 不需要在 main() 主循环里为"utils 状态"单独加一个字段来传递。
            if _hotspot_in_utils:
                if _is_multi:
                    spans = []
                    for n in target_names:
                        _, s, e = extract_kernel_function(context_text, n)
                        if s is None:
                            return {"action": action, "speedup": 1.0,
                                    "error": f"无法从 utils/polybench.c 提取 {n}",
                                    "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                                    "strategy": strategy,
                                    "flags": [], "source": None, "perf_stats": {}}
                        spans.append((n, s, e))
                    rewritten_utils_text = _splice_multi_spans(context_text, spans, multi_chunks)
                    if rewritten_utils_text is None:
                        return {"action": action, "speedup": 1.0,
                                "error": "多函数拼接失败：解析出的函数名和原始 span 对不上",
                                "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                                "strategy": strategy,
                                "flags": [], "source": None, "perf_stats": {}}
                else:
                    _, u_start, u_end = extract_kernel_function(context_text, target_name)
                    if u_start is None:
                        return {"action": action, "speedup": 1.0,
                                "error": f"无法从 utils/polybench.c 提取 {target_name}",
                                "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                                "strategy": strategy,
                                "flags": [], "source": None, "perf_stats": {}}
                    u_start = _widen_span_for_proto_guard(context_text, u_start)
                    rewritten_utils_text = context_text[:u_start] + raw_kernel + context_text[u_end:]
                _driver_unchanged = tmpdir / f"{name}_driver_unchanged.c"
                _driver_unchanged.write_text(base_src_text)
                res = _eval_utils_rewrite(
                    clang, str(_driver_unchanged), Path(utils), rewritten_utils_text,
                    tmpdir, "rw_utils", source_dir, runs, pin_cpu, epsilon,
                    ev.get("correctness_mode", "auto"),
                )
                if not res["ok"]:
                    return {"action": action, "speedup": 1.0, "error": res["error"][:400],
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}
                sp = res["speedup"]
                # 只有真的超过当前最优才落盘——不是"比baseline强就写"。utils.c
                # 是共享状态，本次 run 后续每一步 base="current_best" 都直接读盘上
                # 这份文件；之前用 `sp > 1.0` 当门槛，一个 3.8x（差于当前最优
                # 4.587x 但仍强于 baseline）的重写会真的把磁盘上的文件覆盖成
                # 更差的版本，且这个降级幅度如果小于灾难性回退阈值（默认20%）
                # 根本不会被上层 main() 的回退逻辑捕捉到——历史最优的数字还在，
                # 但磁盘上实际参与后续编译的代码已经悄悄变差了。
                _target_label = "+".join(target_names) if _is_multi else target_name
                if sp > history.best_speedup:
                    (Path(utils) / "polybench.c").write_text(rewritten_utils_text)
                    print(f"  [utils 持久化] {_target_label} 的改写已写回 {utils}/polybench.c，后续步骤生效")
                elif sp > 1.0:
                    print(f"  [utils 未持久化] {sp:.3f}x 强于 baseline 但弱于当前最优 "
                          f"{history.best_speedup:.3f}x，不写回磁盘，避免后续步骤在更差的版本上继续")
                return {"action": action, "speedup": sp, "strategy": f"rewrite(utils/{_target_label}): {strategy}",
                        "error": "", "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                        "flags": [], "source": None,  # driver 文件没变，不更新 current_best_source
                        "perf_stats": {}, "snapshot_path": ""}

            if _is_multi:
                spans = []
                for n in target_names:
                    _, s, e = extract_kernel_function(base_src_text, n)
                    if s is None:
                        return {"action": action, "speedup": 1.0,
                                "error": f"无法从 base source 提取 {n}",
                                "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                                "strategy": strategy,
                                "flags": [], "source": None, "perf_stats": {}}
                    spans.append((n, s, e))
                rewritten_text = _splice_multi_spans(base_src_text, spans, multi_chunks)
                if rewritten_text is None:
                    return {"action": action, "speedup": 1.0,
                            "error": "多函数拼接失败：解析出的函数名和原始 span 对不上",
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}
            else:
                _, start_idx, end_idx = extract_kernel_function(base_src_text, target_name)
                if start_idx is None:
                    return {"action": action, "speedup": 1.0,
                            "error": f"无法从 base source 提取 {target_name}",
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}
                start_idx = _widen_span_for_proto_guard(base_src_text, start_idx)
                rewritten_text = base_src_text[:start_idx] + raw_kernel + base_src_text[end_idx:]
            _rsrc = tmpdir / f"{name}_rewrite.c"
            _rsrc.write_text(rewritten_text)

            # 正确性 + 计时（先纯源码，SMALL + STANDARD 双重验证）
            res = _eval_build_and_time(
                clang, str(_rsrc), src_original,
                extra_flags=[],
                tmpdir=tmpdir, tag="rw",
                utils=utils, source_dir=source_dir,
                runs=runs, pin_cpu=pin_cpu, epsilon=epsilon,
                snapshot_dir=snapshot_dir, step_num=step_num, action=action,
                correctness_mode=ev.get("correctness_mode", "auto"),
            )
            if not res["ok"]:
                err_msg = res["error"]
                # precision-fix/compile-fix sub-attempts below both build a fix
                # prompt from `raw_kernel`, which is None on the multi-target
                # path (multi_chunks carries the result there instead, see
                # _do_rewrite_source's Phase 2) -- not extending those two
                # sub-flows to the multi-function case for now, just skip them
                # and report the plain error; this only affects the (rare in
                # practice) driver-file multi-target path, not the in_utils one.
                # ── precision-fix sub-attempt (same as legacy run_source_round) ──
                if not _is_multi and ("precision" in err_msg.lower() or "数值不一致" in err_msg or "mismatch" in err_msg.lower()):
                    print(f"  精度失败：尝试 precision-fix LLM...")
                    orig_kernel_txt, _, _ = extract_kernel_function(
                        base_src_text, target_name)
                    diagnosis = analyze_precision_failure(
                        llm, orig_kernel_txt or base_src_text, raw_kernel, err_msg)
                    if diagnosis:
                        print("  [精度分析] " + diagnosis.splitlines()[0][:120])
                    fix_prompt = _build_precision_fix_prompt(
                        target_name, orig_kernel_txt or base_src_text,
                        raw_kernel, err_msg, diagnosis=diagnosis)
                    fix_resp = llm.call([
                        {"role": "system",
                         "content": "You are a C performance expert. Fix the precision error. "
                                    "Output C code only."},
                        {"role": "user", "content": fix_prompt},
                    ], max_tokens=max_tokens)
                    if fix_resp:
                        fixed_kernel = _strip_fences(fix_resp)
                        _, fs_idx, fe_idx = extract_kernel_function(base_src_text, target_name)
                        if fs_idx is not None:
                            fixed_text = base_src_text[:fs_idx] + fixed_kernel + base_src_text[fe_idx:]
                            fixed_src = tmpdir / f"{name}_rw_fix.c"
                            fixed_src.write_text(fixed_text)
                            res_fix = _eval_build_and_time(
                                clang, str(fixed_src), src_original,
                                extra_flags=[],
                                tmpdir=tmpdir, tag="rw_fix",
                                utils=utils, source_dir=source_dir,
                                runs=runs, pin_cpu=pin_cpu, epsilon=epsilon,
                                correctness_mode=ev.get("correctness_mode", "auto"),
                            )
                            if res_fix["ok"]:
                                print(f"  [精度修复] 通过！继续计时...")
                                res = res_fix
                                rewritten_text = fixed_text
                                _rsrc = fixed_src
                            else:
                                print(f"  [精度修复] 仍失败: {res_fix['error'][:80]}")
                                err_msg = (f"precision error (fix also failed): "
                                           f"{err_msg[:200]}\nROOT CAUSE: {diagnosis[:150]}")
                # ── generic compile-fix sub-attempt (non-precision compile/link errors) ──
                elif not _is_multi and ("编译失败" in err_msg or "链接" in err_msg or "undefined reference" in err_msg.lower()):
                    print(f"  编译失败：尝试 compile-fix LLM...")
                    orig_kernel_txt, _, _ = extract_kernel_function(
                        base_src_text, target_name)
                    fix_prompt = _build_compile_fix_prompt(
                        target_name, orig_kernel_txt or base_src_text, raw_kernel, err_msg)
                    fix_resp = llm.call([
                        {"role": "system",
                         "content": "You are a C compiler expert. Fix ONLY the compile "
                                    "error, keep the rest of the code unchanged as much "
                                    "as possible. Output C code only."},
                        {"role": "user", "content": fix_prompt},
                    ], max_tokens=max_tokens)
                    if fix_resp:
                        fixed_kernel = _strip_fences(fix_resp)
                        _, fs_idx, fe_idx = extract_kernel_function(base_src_text, target_name)
                        if fs_idx is not None:
                            fixed_text = base_src_text[:fs_idx] + fixed_kernel + base_src_text[fe_idx:]
                            fixed_src = tmpdir / f"{name}_rw_cfix.c"
                            fixed_src.write_text(fixed_text)
                            res_fix = _eval_build_and_time(
                                clang, str(fixed_src), src_original,
                                extra_flags=[],
                                tmpdir=tmpdir, tag="rw_cfix",
                                utils=utils, source_dir=source_dir,
                                runs=runs, pin_cpu=pin_cpu, epsilon=epsilon,
                                correctness_mode=ev.get("correctness_mode", "auto"),
                            )
                            if res_fix["ok"]:
                                print(f"  [编译修复] 通过！继续计时...")
                                res = res_fix
                                rewritten_text = fixed_text
                                _rsrc = fixed_src
                            else:
                                print(f"  [编译修复] 仍失败: {res_fix['error'][:80]}")
                                err_msg = f"compile error (fix also failed): {err_msg[:300]}"
                if not res["ok"]:
                    return {"action": action, "speedup": 1.0, "error": err_msg[:400],
                            "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                            "strategy": strategy,
                            "flags": [], "source": None, "perf_stats": {}}

            sp_src_only = res["speedup"]
            source      = rewritten_text
            flags       = []
            perf        = res.get("perf_stats", {})
            snap        = res.get("snapshot_path", "")
            print(f"  rewrite_source（纯源码 -O3）: {sp_src_only:.3f}x  [{strategy}]")

            # ── 始终用 current_best_flags 测组合版（如果有）─────────────────────
            extra = also_flags or current_best_flags
            sp    = sp_src_only
            sp_with_flags = None
            if extra:
                res2 = _eval_build_and_time(
                    clang, str(_rsrc), src_original,
                    extra_flags=extra,
                    tmpdir=tmpdir, tag="rw_flags",
                    utils=utils, source_dir=source_dir,
                    runs=runs, pin_cpu=pin_cpu, epsilon=epsilon,
                )
                if res2["ok"]:
                    sp_with_flags = res2["speedup"]
                    flags_str = " ".join(extra)
                    if sp_with_flags > sp_src_only:
                        sp   = sp_with_flags
                        flags = extra
                        perf  = res2.get("perf_stats", perf)
                        print(f"  rewrite_source + [{flags_str}]: {sp_with_flags:.3f}x  ← 组合更优，保留参数")
                    else:
                        # flags hurt this source — don't carry them forward for this source
                        # but don't erase current_best_flags globally (main loop handles that)
                        print(f"  rewrite_source + [{flags_str}]: {sp_with_flags:.3f}x  "
                              f"(比纯源码差，此版本不用参数)")

            strat = f"rewrite: {strategy}"
            return {"action": action, "speedup": sp, "strategy": strat, "error": "",
                    "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                    "flags": flags, "source": source,
                    "perf_stats": perf, "snapshot_path": snap,
                    "speedup_src_only": sp_src_only,
                    "speedup_with_flags": sp_with_flags}

        # ── dispatch ──────────────────────────────────────────────────────────
        if action == "try_flags":
            return _do_try_flags()
        elif action == "try_pragma":
            return _do_try_pragma()
        elif action == "rewrite_source":
            return _do_rewrite_source()
        else:
            return {"action": action, "speedup": 1.0, "error": f"未知 action: {action}",
                    "reasoning": reasoning, "improvement_analysis": improvement_analysis,
                    "strategy": "", "flags": [], "source": None,
                    "perf_stats": {}}




# ── Parameter-tuning round ───────────────────────────────────────────────────

def run_param_round(src: str, config, llm: LLMClient,
                    runs: int, pin_cpu, top_k: int = 4,
                    kernel_name: str = None) -> dict:
    """
    Full parameter tuning round: dynamic discovery → 2-LLM-call refinement → adaptive search.
    kernel_name: override for when src filename differs from actual kernel (re-tuning on opt copy).
    Returns dict with baseline_ms, best_flags_list, best_flags_str, best_time_ms, best_speedup.
    """
    name = Path(src).stem
    if kernel_name is None:
        kernel_name = f"kernel_{name.replace('-', '_')}"
    utils       = find_polybench_utilities(src)
    if not utils:
        print("  utils not found, skipping param round"); return {}
    source_dir  = Path(src).resolve().parent
    polybench_c = utils / "polybench.c"
    clang       = config.compiler.clang_path
    opt         = config.compiler.opt_path

    # Step 1: kernel line range
    kline_start, kline_end = get_kernel_line_range(src, kernel_name)
    print(f"  Kernel '{kernel_name}': lines {kline_start}–{kline_end}")

    # Step 2: collect remarks, filter to kernel lines only
    print("  Collecting optimization remarks (filtered to kernel lines)...")
    all_remarks    = extract_remarks_by_pass(clang, src, utils, source_dir)
    kernel_remarks = filter_remarks_to_kernel(all_remarks, kline_start, kline_end)

    # Step 3: extract O3 passes for kernel
    print("  Extracting O3 passes via -debug-pass-manager...")
    runner = CompilerRunner(config, None)
    ok, o0_ir, err = runner.extract_ir(src)
    if not ok:
        print(f"  IR extraction failed: {err}"); return {}
    kernel_passes  = extract_o3_passes_for_kernel(opt, o0_ir, kernel_name)
    kernel_ir_text = extract_kernel_ir(o0_ir, kernel_name, max_lines=150)
    print(f"  {len(kernel_passes)} unique passes ran on {kernel_name}")

    # Step 4: discover options from opt --help-hidden for passes that actually ran
    print("  Discovering tunable options from opt --help-hidden...")
    discovered_opts = discover_options_from_help(opt, kernel_passes)

    # Step 5: score passes
    scored = score_passes(kernel_passes, kernel_remarks, discovered_opts)
    if not scored:
        # Fallback: passes with options even if no kernel remarks
        for pname, opts in discovered_opts.items():
            if pname in kernel_passes:
                cat_name, cat_weight = PASS_CATEGORIES.get(pname, ("other", 2))
                scored.append((cat_weight, pname, cat_name, 0, []))
        scored.sort(reverse=True, key=lambda x: x[0])
    if not scored:
        print("  No tunable passes found, skipping."); return {}
    top_passes = scored[:top_k]

    # Step 6: IR stats before/after candidate probe values
    # Use clang -O3 (same as real build)
    print("  Measuring IR stats before/after probe values...")
    try:
        os.unlink(o0_ir)
    except Exception:
        pass

    baseline_stats = get_ir_stats(clang, src, utils, source_dir, kernel_name)
    ir_diff_info   = []
    for _, pname, cat, mc, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        for opt_entry in opts[:2]:
            flag      = opt_entry["flag"].lstrip("-")
            probe_val = _pick_probe_value(flag)
            if probe_val is None:
                continue
            stats_mod = get_ir_stats(clang, src, utils, source_dir, kernel_name,
                                     extra_mllvm=[f"-{flag}={probe_val}"])
            if baseline_stats and stats_mod:
                delta_vec   = stats_mod.get("vector_ops", 0) - baseline_stats.get("vector_ops", 0)
                delta_instr = stats_mod.get("total_instr", 0) - baseline_stats.get("total_instr", 0)
                if delta_vec != 0 or delta_instr != 0:
                    ir_diff_info.append(
                        f"  -{flag}={probe_val}: vector_ops "
                        f"{baseline_stats.get('vector_ops',0)} → {stats_mod.get('vector_ops',0)}"
                        f" (Δ{delta_vec:+d}), total_instr Δ{delta_instr:+d}"
                    )

    # Step 7: build LLM prompt
    cpu_info = get_cpu_info()
    missed_block = []
    for _, pname, _, mc, entries in top_passes:
        missed_only = [e for e in entries if e["type"] == "missed"]
        if missed_only:
            missed_block.append(f"\n**{pname}** (kernel misses: {mc})")
            for e in missed_only[:4]:
                missed_block.append(f"  {e['file']}:{e['line']} → \"{e['msg'][:90]}\"")

    opts_block = []
    for _, pname, cat, mc, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        if not opts:
            continue
        opts_block.append(f"\n**{pname}** [category={cat}, kernel_misses={mc}]")
        for o in opts:
            opts_block.append(f"  {o['flag']}=<{o['type']}>  — {o['desc']}")

    prompt = f"""You are an expert LLVM compiler optimization engineer.
Your job: pick ≤3 -mllvm threshold flags and candidate values to improve {name} performance.

## Hardware
{cpu_info}

## Kernel (O0 IR excerpt — before any optimization)
```llvm
{kernel_ir_text}
```

## Ranked tunable passes (scored by kernel-only missed remarks × category weight)
{chr(10).join(f'{i+1}. score={sc:.1f}  {pn} [{cat}, {mc} kernel misses]'
              for i, (sc, pn, cat, mc, _) in enumerate(top_passes))}

## Missed remarks from kernel lines ONLY (IO/printf lines excluded)
{"(none)" if not missed_block else chr(10).join(missed_block)}

## IR change evidence (baseline vs. probing one candidate value per flag)
Baseline stats: {baseline_stats}
{"(no IR changes observed)" if not ir_diff_info else chr(10).join(ir_diff_info)}

## Available tunable flags per pass (from opt --help-hidden, numeric threshold only)
{chr(10).join(opts_block) if opts_block else "(none discovered)"}

## Selection rules
1. MANDATORY: Select ONE flag from EACH pass listed in "Available tunable flags".
   If a pass has multiple flags, pick the most impactful one for that pass.
   Minimum 5 flags total — if fewer than 5 passes have options, pick all of them.
2. ONLY choose flags from the "Available tunable flags" list above.
3. NEVER choose: force-vector-width, force-vector-interleave, unroll-count, or any
   "force-" / "disable-" flag — these bypass the cost model and cause regressions.
4. Prefer flags where IR change evidence shows vector_ops increase (Δvec > 0).
5. If a miss says "cost 0 >= 0", the threshold flag with a NEGATIVE value is the fix.
6. ≤4 candidates per flag. Each flag is searched INDEPENDENTLY (not as a joint grid).
7. Include the current default value as one candidate.

Output strict JSON (no markdown):
{{
  "analysis": "<2 sentences: key bottlenecks and why these flags>",
  "parameters": [
    {{"flag": "-<flag-from-available-list>", "candidates": [<val1>, <val2>, <val3>, <default>]}},
    {{"flag": "-<another-flag>", "candidates": [<val1>, <val2>, <default>]}}
  ]
}}"""

    print("  Querying LLM for flag selection...")
    resp = llm.call([
        {"role": "system",
         "content": "You are a compiler parameter tuning system. Output strict JSON only."},
        {"role": "user", "content": prompt},
    ])
    if not resp:
        print("  LLM no response"); return {}

    try:
        parsed = json.loads(strip_json_fences(resp))
    except Exception as e:
        print(f"  JSON parse error: {e}"); return {}

    params   = parsed.get("parameters", [])
    analysis = parsed.get("analysis", "")
    if not params:
        print("  LLM returned no parameters"); return {}
    print(f"  Analysis: {analysis}")
    for p in params:
        print(f"    {p['flag']}: {p['candidates']}")

    # Guarantee coverage: add one heuristic flag per top pass not already selected
    selected_flags = {p["flag"] for p in params}
    for _, pname, _, _, _ in top_passes:
        opts = discovered_opts.get(pname, [])
        added = False
        for o in opts:
            flag = o["flag"].replace("--", "-")
            if flag in selected_flags:
                added = True
                break
            inferred = _infer_candidates_from_desc(
                flag, o.get("type", "uint"), o.get("desc", ""))
            if inferred:
                default_cands = inferred[:6]
                params.append({"flag": flag, "candidates": default_cands})
                selected_flags.add(flag)
                print(f"    [auto] {flag}: {default_cands[:4]}")
                added = True
                break
        if not added and opts:
            # No probe value heuristic: use first option with simple range
            o = opts[0]
            flag = o["flag"].replace("--", "-")
            if flag not in selected_flags:
                params.append({"flag": flag, "candidates": [4, 8, 16, 32]})
                selected_flags.add(flag)
                print(f"    [auto-range] {flag}: [4, 8, 16, 32]")
    print(f"  Total flags to search: {len(params)}")

    # Step 8: adaptive grid search (Phase A: independent, Phase B: joint top-2)
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        baseline_bin = tmpdir / "baseline"
        ok, err = compile_binary(clang, src, polybench_c, utils, source_dir, baseline_bin)
        if not ok:
            print(f"  Baseline compile failed: {clean_clang_diagnostics(err, max_diagnostics=3)}"); return {}
        baseline_time = tp_run_timing(str(baseline_bin), runs=runs, pin_cpu=pin_cpu)
        if baseline_time <= 0:
            print("  Baseline timing failed"); return {}
        print(f"  Baseline -O3: {baseline_time:.2f} ms")

        # Phase A: independent flag search
        print("  Phase A — independent flag search:")
        best_per_flag = {}
        all_results   = []

        for p_idx, p in enumerate(params):
            flag_name  = p["flag"]
            candidates = p["candidates"]
            best_t     = baseline_time
            best_v     = None
            for val in candidates:
                mllvm = ["-mllvm", f"{flag_name}={val}"]
                cand  = tmpdir / f"phA_{p_idx}_{val}"
                ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir,
                                       cand, extra_flags=mllvm)
                if not ok:
                    print(f"    {flag_name}={val} → compile FAILED")
                    continue
                t  = tp_run_timing(str(cand), runs=runs, pin_cpu=pin_cpu)
                if t <= 0:
                    continue
                sp   = baseline_time / t
                mark = " ← best" if t < best_t else ""
                print(f"    {flag_name}={val} → {t:.1f} ms ({sp:.3f}x){mark}")
                all_results.append({"flags": f"{flag_name}={val}", "time_ms": t, "speedup": sp})
                if t < best_t:
                    best_t = t; best_v = val
            best_per_flag[flag_name] = (best_t, best_v)

        # LLM refinement: show Phase A results, ask for refined candidates
        phase_a_summary = []
        for p in params:
            fn = p["flag"]
            t_best, v_best = best_per_flag.get(fn, (baseline_time, None))
            sp_best = baseline_time / t_best if t_best > 0 else 1.0
            per_val = [r for r in all_results if r["flags"].startswith(fn.lstrip("-") + "=")
                       or r["flags"].startswith(fn + "=")]
            lines = [f"  {r['flags']} → {r['time_ms']:.1f} ms ({r['speedup']:.3f}x)" for r in per_val]
            phase_a_summary.append(f"\n{fn}: best={v_best} ({sp_best:.3f}x)\n" + "\n".join(lines))

        refine_prompt = f"""Phase A independent grid search on {name} is complete.
Baseline: {baseline_time:.2f} ms

Phase A results:
{"".join(phase_a_summary)}

Based on these ACTUAL measured results:
1. Which flags showed clear improvement (speedup > 1.01)?
2. Which flags were harmful or neutral (speedup < 1.0 or negligible)?
3. For each beneficial flag, narrow the candidate range around its best value.
4. Drop flags that showed no improvement.
5. If joint combinations might help, suggest ≤3 flags with ≤4 candidates each.

RULES:
- Keep flags that showed speedup > 1.005x in Phase A (beneficial).
- For each kept flag, search ±1 step around its best Phase A value (narrow the range).
- Drop flags that were uniformly worse (speedup < 0.995x for all candidates).
- Keep neutral flags (0.995–1.005x) only if IR evidence suggested they should help.
- For Phase B joint search, ≤4 candidates per kept flag is fine (searched independently).

Output strict JSON:
{{
  "analysis": "<2 sentences on what Phase A revealed and why>",
  "parameters": [{{"flag": "-flag-name", "candidates": [val1, val2, val3]}}]
}}"""

        print("  LLM refinement (Phase A → refined candidates)...")
        resp2 = llm.call([
            {"role": "system", "content": "Output strict JSON only."},
            {"role": "user",   "content": refine_prompt},
        ])
        refined_params = params  # fallback to original
        if resp2:
            try:
                p2 = json.loads(strip_json_fences(resp2))
                if p2.get("parameters"):
                    refined_params = p2["parameters"]
                    analysis = p2.get("analysis", analysis)
                    print(f"  Refined: {[(p['flag'], p['candidates']) for p in refined_params]}")
            except Exception:
                print("  Refinement JSON parse failed, using original params")
        else:
            print("  LLM refinement no response, using original params")

        # Phase B: joint search on best pairs
        # Seed with the single best Phase A result (one flag only, actually tested)
        print("  Phase B — joint search (best pairs):")
        best_phA_entry   = min(best_per_flag.items(), key=lambda kv: kv[1][0])
        best_joint_time  = best_phA_entry[1][0]
        best_joint_combo = {best_phA_entry[0]: best_phA_entry[1][1]}

        if len(refined_params) >= 2:
            # Use refined_params for Phase B (LLM already narrowed the range)
            top2 = sorted(
                [(p["flag"], best_per_flag.get(p["flag"], (baseline_time, None)))
                 for p in refined_params],
                key=lambda x: x[1][0]
            )[:2]
            joint_search = []
            for fn, (bt, bv) in top2:
                if bv is None:
                    continue
                # Use refined candidates (already narrowed by LLM)
                ref_entry = next((p for p in refined_params if p["flag"] == fn), None)
                near = ref_entry["candidates"] if ref_entry else [bv]
                joint_search.append((fn, near))

            for combo in itertools.product(*[vs for _, vs in joint_search]):
                flags = []
                desc  = []
                for (fn, _), val in zip(joint_search, combo):
                    flags += ["-mllvm", f"{fn}={val}"]
                    desc.append(f"{fn}={val}")
                desc_str = ", ".join(desc)
                cand = tmpdir / f"phB_{'_'.join(str(v) for v in combo)}"
                ok, _ = compile_binary(clang, src, polybench_c, utils, source_dir,
                                       cand, extra_flags=flags)
                if not ok:
                    continue
                t  = tp_run_timing(str(cand), runs=runs, pin_cpu=pin_cpu)
                if t <= 0:
                    continue
                sp   = baseline_time / t
                mark = " ← BEST" if t < best_joint_time else ""
                print(f"    {desc_str} → {t:.1f} ms ({sp:.3f}x){mark}")
                all_results.append({"flags": desc_str, "time_ms": t, "speedup": sp})
                if t < best_joint_time:
                    best_joint_time  = t
                    best_joint_combo = {fn: val
                                        for (fn, _), val in zip(joint_search, combo)}

    best_speedup = baseline_time / best_joint_time if best_joint_time < baseline_time else 1.0
    if best_speedup > 1.0:
        best_flags_list = []
        for fn, val in best_joint_combo.items():
            if val is not None:
                best_flags_list += ["-mllvm", f"{fn}={val}"]
        best_flags_str = " ".join(f"-mllvm {fn}={val}"
                                  for fn, val in best_joint_combo.items()
                                  if val is not None)
        print(f"  Best: {best_flags_str}  → {best_joint_time:.2f} ms ({best_speedup:.3f}x)")
    else:
        best_flags_list = []
        best_flags_str  = None
        best_joint_time = baseline_time
        print("  No improvement over -O3 baseline.")

    result = {
        "program":       name,
        "baseline_ms":   baseline_time,
        "analysis":      analysis,
        "parameters":    params,
        "results":       all_results,
        "best_flags":    best_flags_str,
        "best_flags_list": best_flags_list,
        "best_time_ms":  best_joint_time,
        "best_speedup":  best_speedup,
    }
    out_dir = Path("outputs"); out_dir.mkdir(exist_ok=True)
    with open(out_dir / f"{name}_param_results.json", "w") as f:
        json.dump(result, f, indent=2)
    return result


# ── Source-rewrite round ─────────────────────────────────────────────────────

def run_source_round(rewrite_src: str, ref_src: str, config, llm: LLMClient,
                     runs: int, pin_cpu, epsilon: float, max_tokens: int,
                     vect_remarks: list, cpu_cache_info: str,
                     prev_error: "str | None", attempt: int,
                     param_extra_flags: list = None,
                     param_analysis: str = "",
                     extra_hints: str = "",
                     rich_remarks: "dict | None" = None,
                     precision_failures: "list | None" = None,
                     attempt_history: "list | None" = None) -> dict:
    """
    Single LLM source-rewrite attempt.

    rewrite_src : source the LLM rewrites — the CURRENT BEST version (may be a temp file).
    ref_src     : ORIGINAL source — used for correctness reference and baseline timing.
                  Also used to locate polybench utilities and the benchmark header.

    Returns {'status': 'ok'|'compile_error'|'precision_error'|'timing_error'|'no_response',
             'speedup': float, 'time_ms': float, 'source': str, 'error': str}
    Speedup is always relative to ref_src (original -O3 baseline).
    """
    name        = Path(ref_src).stem          # always from original
    kernel_name = f"kernel_{name.replace('-', '_')}"
    utils       = find_polybench_utilities(ref_src)
    if not utils:
        return {"status": "compile_error", "error": "utils not found", "speedup": 1.0}
    source_dir   = Path(ref_src).resolve().parent
    polybench_c  = utils / "polybench.c"
    clang        = config.compiler.clang_path
    include_dirs = [utils, source_dir]

    with open(rewrite_src) as f:
        source_code = f.read()
    current_kernel, start_idx, end_idx = extract_kernel_function(source_code, kernel_name)
    if not current_kernel:
        return {"status": "compile_error",
                "error": f"Cannot extract {kernel_name} from rewrite source", "speedup": 1.0}
    macro_names = extract_header_macros(ref_src)

    param_ctx = ""
    if param_analysis:
        param_ctx = (f"\n### Compiler param analysis (from initial param round)\n"
                     f"{param_analysis.strip()}\n")
    if param_extra_flags:
        param_ctx += f"All timing builds use: {' '.join(param_extra_flags)}\n"

    prompt = _build_prompt(kernel_name, current_kernel, macro_names,
                           vect_remarks, prev_error, attempt,
                           cpu_cache_info=cpu_cache_info,
                           param_context=param_ctx,
                           extra_hints=extra_hints,
                           rich_remarks=rich_remarks,
                           precision_history=precision_failures or None,
                           attempt_history=attempt_history or None)
    # Use low temperature for deterministic, high-quality rewrites.
    # Each round builds on the previous best, so diversity comes from the
    # iterative improvement loop, not from random sampling.
    response = llm.call([
        {"role": "system", "content": "You are a performance code rewriter. Output C code only."},
        {"role": "user",   "content": prompt},
    ], max_tokens=max_tokens, temperature=0.2)

    if not response:
        return {"status": "no_response", "error": "LLM no response", "speedup": 1.0}

    opt_code   = _strip_fences(response)
    opt_source = source_code[:start_idx] + opt_code + source_code[end_idx:]

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        opt_src_path = tmpdir / f"{name}_opt.c"
        with open(opt_src_path, "w") as f:
            f.write(opt_source)

        # Correctness reference always from ORIGINAL (ref_src)
        orig_dump_bin = tmpdir / "orig_dump"
        ok, err = compile_c(clang, [ref_src, str(polybench_c)], include_dirs,
                            ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"], orig_dump_bin)
        if not ok:
            return {"status": "compile_error",
                    "error": f"Ref compile: {clean_clang_diagnostics(err, max_diagnostics=3)}", "speedup": 1.0}
        res_ref  = subprocess.run([str(orig_dump_bin)], capture_output=True, text=True, timeout=30)
        ref_nums = extract_numbers_from_dump((res_ref.stdout or "") + (res_ref.stderr or ""))

        # Optimized dump (no param flags — clean correctness check)
        opt_dump_bin = tmpdir / "opt_dump"
        ok, cerr = compile_c(clang, [str(opt_src_path), str(polybench_c)], include_dirs,
                             ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"], opt_dump_bin)
        if not ok:
            return {"status": "compile_error",
                    "error": f"COMPILE ERROR:\n{clean_clang_diagnostics(cerr)}", "speedup": 1.0}
        try:
            res_opt = subprocess.run([str(opt_dump_bin)], capture_output=True,
                                     text=True, timeout=30)
        except subprocess.TimeoutExpired:
            return {"status": "timing_error", "error": "Dump timed out", "speedup": 1.0}

        opt_nums = extract_numbers_from_dump((res_opt.stdout or "") + (res_opt.stderr or ""))
        ok_eq, msg = compare_outputs(ref_nums, opt_nums, epsilon=epsilon)
        if not ok_eq:
            # ── precision-fix sub-attempt ─────────────────────────────────────
            print(f"  Precision FAILED: {msg[:120]}")
            print("  [Precision Analysis] Diagnosing root cause...")
            diagnosis = analyze_precision_failure(llm, current_kernel, opt_code, msg)
            if diagnosis:
                print("  [Precision Analysis] " +
                      "\n    ".join(diagnosis.splitlines()[:5]))
            if precision_failures is not None:
                precision_failures.append({
                    "attempt": attempt, "code": opt_code,
                    "error": msg, "diagnosis": diagnosis,
                })
            print("  [Precision Fix] Querying LLM...")
            fix_prompt = _build_precision_fix_prompt(
                kernel_name, current_kernel, opt_code, msg,
                diagnosis=diagnosis,
                precision_history=(precision_failures[:-1] if precision_failures else None),
            )
            fix_resp = llm.call([
                {"role": "system",
                 "content": "You are a C performance expert. Fix the precision error. "
                            "Output C code only."},
                {"role": "user", "content": fix_prompt},
            ], max_tokens=max_tokens)
            precision_fixed = False
            if fix_resp:
                fixed_code   = _strip_fences(fix_resp)
                fixed_source = source_code[:start_idx] + fixed_code + source_code[end_idx:]
                fixed_src    = tmpdir / "opt_fix.c"
                fixed_src.write_text(fixed_source)
                fix_dump_bin = tmpdir / "fix_dump"
                ok2, _err2 = compile_c(
                    clang, [str(fixed_src), str(polybench_c)], include_dirs,
                    ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"], fix_dump_bin)
                if ok2:
                    try:
                        res_fix  = subprocess.run([str(fix_dump_bin)],
                                                  capture_output=True, text=True,
                                                  timeout=30, errors="replace")
                        fix_out  = (res_fix.stdout or "") + (res_fix.stderr or "")
                        fix_nums = extract_numbers_from_dump(fix_out)
                        ok_fix, fix_msg = compare_outputs(ref_nums, fix_nums, epsilon=epsilon)
                        if ok_fix:
                            print(f"  [Precision Fix] PASSED")
                            opt_code     = fixed_code
                            opt_source   = fixed_source
                            opt_src_path = fixed_src
                            precision_fixed = True
                        else:
                            print(f"  [Precision Fix] Still FAILED: {fix_msg[:80]}")
                            if precision_failures is not None:
                                precision_failures.append({
                                    "attempt": f"{attempt}fix", "code": fixed_code,
                                    "error": fix_msg, "diagnosis": "",
                                })
                    except subprocess.TimeoutExpired:
                        print("  [Precision Fix] Timeout.")
                else:
                    print("  [Precision Fix] Compile FAILED.")
            if not precision_fixed:
                return {"status": "precision_error",
                        "error": (f"PRECISION ERROR: {msg}\n"
                                  f"ROOT CAUSE: {diagnosis[:200] if diagnosis else 'unknown'}"),
                        "speedup": 1.0}

        # Baseline timing always from ORIGINAL (ref_src) with param flags
        orig_time_bin = tmpdir / "orig_time"
        ok, _ = compile_c(clang, [ref_src, str(polybench_c)], include_dirs,
                          ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"], orig_time_bin,
                          extra_flags=(param_extra_flags or []))
        orig_time = ts_run_timing(str(orig_time_bin), runs=runs, pin_cpu=pin_cpu) if ok else -1.0

        # Optimized timing (with param flags — compound source+param speedup)
        opt_time_bin = tmpdir / "opt_time"
        ok, err = compile_c(clang, [str(opt_src_path), str(polybench_c)], include_dirs,
                            ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"], opt_time_bin,
                            extra_flags=(param_extra_flags or []))
        if not ok:
            return {"status": "timing_error",
                    "error": f"Time compile failed: {clean_clang_diagnostics(err, max_diagnostics=3)}", "speedup": 1.0}
        opt_time = ts_run_timing(str(opt_time_bin), runs=runs, pin_cpu=pin_cpu)
        if opt_time <= 0:
            return {"status": "timing_error", "error": "No timing output", "speedup": 1.0}

        speedup = (orig_time / opt_time) if orig_time > 0 else 1.0
        return {"status": "ok", "speedup": speedup, "time_ms": opt_time,
                "orig_time_ms": orig_time, "source": opt_source, "error": ""}


# ── Main ─────────────────────────────────────────────────────────────────────

def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="COMET agent optimizer")
    parser.add_argument("--program",       required=True)
    parser.add_argument("--config",        default="configs")
    parser.add_argument("--max-steps",     type=int,   default=15,
                        help="Max agent steps (default=15); LLM may stop earlier")
    parser.add_argument("--rounds",        type=int,   default=None,
                        help="Alias for --max-steps (backwards compat)")
    parser.add_argument("--runs",          type=int,   default=5)
    parser.add_argument("--pin-cpu",       type=int,   default=None)
    parser.add_argument("--epsilon",       type=float, default=1e-4)
    parser.add_argument("--max-tokens",    type=int,   default=12000)
    parser.add_argument("--top-k",         type=int,   default=8)
    parser.add_argument("--dataset",       default="auto",
                        help="Dataset type: auto|polybench|tsvc|cbench (default=auto)")
    parser.add_argument("--no-log",        action="store_true",
                        help="Disable run logger (no runs/ directory)")
    # Mode flags
    parser.add_argument("--unified-only",  action="store_true",
                        help="Run only the agent optimization (skip legacy param/source)")
    parser.add_argument("--graph-only",    action="store_true",
                        help="Generate pass pipeline graph and exit")
    # Legacy mode flags (kept for backwards-compat)
    parser.add_argument("--param-only",    action="store_true")
    parser.add_argument("--source-only",   action="store_true")
    return parser


def main():
    args = _build_arg_parser().parse_args()

    max_steps = args.rounds if args.rounds is not None else args.max_steps

    loader  = ConfigLoader(config_dir=os.path.abspath(args.config))
    config  = loader.load_all()
    pin_cpu = args.pin_cpu if args.pin_cpu is not None else config.runtime.pin_cpu
    clang   = config.compiler.clang_path
    _llm    = LLMClient(config.llm)

    # ── Dataset detection ─────────────────────────────────────────────────
    src = args.program
    if not os.path.exists(src):
        sys.exit(f"Not found: {src}")
    dataset_type = (detect_dataset_type(src)
                    if args.dataset == "auto" else args.dataset)
    name        = Path(src).stem
    kernel_name = f"kernel_{name.replace('-', '_')}"

    # ── Run logger: captures stdout + LLM calls ───────────────────────────
    _run_logger: Optional[RunLogger] = None
    llm = _llm
    if not args.no_log:
        try:
            _run_logger = RunLogger(program_name=name, dataset=dataset_type)
            _current_step = [0]
            llm = LoggingLLMClient(_llm, _run_logger,
                                   step_getter=lambda: _current_step[0])
        except Exception as _log_err:
            print(f"[warn] RunLogger init failed: {_log_err}, logging disabled")
    print(f"  Dataset type: {dataset_type}")

    print(f"{'='*60}")
    print(f"COMET agent: {name}  max_steps={max_steps}")
    print(f"{'='*60}")

    runner = CompilerRunner(config, None)

    # ── Per-run output directory: inside run_logger dir when available ────────
    _out_dir: Path = (
        _run_logger.run_dir / "outputs"
        if _run_logger else Path("outputs")
    )
    _out_dir.mkdir(parents=True, exist_ok=True)

    # ── Agent mode (default / --unified-only) ────────────────────────────────
    if args.unified_only or not (args.param_only or args.source_only):
        print("\n[Evidence collection -- one-time analysis]")
        ev = collect_all_evidence(src, config, runner, kernel_name,
                                  output_dir=str(_out_dir))
        if not ev:
            sys.exit("Evidence collection failed (IR extraction error?)")
        _pg_stats = ev.get("pass_graph", {}).get("stats", {})
        print(f"  {_pg_stats.get('unique_passes', '?')} passes in pipeline, "
              f"{len(ev['kernel_passes'])} ran on {kernel_name}")

        # ── 热点函数：整个 run 只选一次，选完缓存进 ev ──────────────────────
        # 必须在 agent 步骤循环开始前做，不能等到某一步的 rewrite_source 处理
        # 里才选：外层 agent 决策 prompt（_build_agent_prompt，决定 action +
        # strategy 那一步）也要用同一个目标，否则它会照着 kernel_name 自己的
        # 证据/源码提 strategy（比如 SPEC mcf_r 提"合并 printf"），而实际写代码、
        # 验证、生效的却是热点函数（primal_iminus），两者对不上等于用错误的
        # 意图指导实现阶段的 LLM——这正是之前观测到的 bug。
        _utils_text_hs = None
        if ev.get("utils"):
            _pc_hs = Path(ev["utils"]) / "polybench.c"
            if _pc_hs.exists():
                try:
                    _utils_text_hs = _pc_hs.read_text(errors="replace")
                except OSError:
                    pass
        _driver_text_hs = Path(src).read_text(errors="replace")
        # select_hotspot_targets (plural): mcf_r's primal_iminus/switch_arcs/
        # markBaskets score within 4% of each other -- the hot loop's cost is
        # genuinely spread across several functions of comparable weight,
        # not concentrated in one, so rewriting only the #1 scorer caps
        # achievable speedup at whatever that one function alone is worth.
        # Reduces to a single-element list (identical behavior to the old
        # select_hotspot_target) for the common case where one function
        # really does dominate -- see src/hotspot.py's docstring.
        _hotspots0 = select_hotspot_targets(kernel_name, _driver_text_hs, _utils_text_hs)
        ev["hotspot_targets"]  = [h["name"] for h in _hotspots0]
        ev["hotspot_target"]   = _hotspots0[0]["name"]
        ev["hotspot_in_utils"] = _hotspots0[0]["in_utils"]
        ev["hotspot_reason"]   = "; ".join(h["reason"] for h in _hotspots0)
        if _hotspots0[0]["name"] != kernel_name:
            _where_hs = ("utils/polybench.c（本次 run 私有可写副本）"
                        if _hotspots0[0]["in_utils"] else "driver 文件")
            if len(_hotspots0) > 1:
                print(f"  [热点筛选] 联合改写目标 = {', '.join(ev['hotspot_targets'])}"
                      f"（{_where_hs}，而非 {kernel_name}）——热点分数彼此接近，"
                      f"分散在多个函数里，需要一起改：")
                for h in _hotspots0:
                    print(f"    - {h['reason']}")
            else:
                print(f"  [热点筛选] 真正的改写目标 = {_hotspots0[0]['name']}"
                      f"（{_where_hs}，而非 {kernel_name}）：{_hotspots0[0]['reason']}")

        # --graph-only: just render the pass graph and exit
        if args.graph_only:
            pg = ev.get("pass_graph", {})
            print(f"\nPass graph: {pg.get('dot_path', 'not generated')}")
            if pg.get("png_path") and Path(pg["png_path"]).exists():
                print(f"PNG:        {pg['png_path']}")
            if pg.get("svg_path") and Path(pg["svg_path"]).exists():
                print(f"SVG:        {pg['svg_path']}")
            if pg.get("summary"):
                print(f"\n{pg['summary']}")
            return

        # Establish baseline once (静态分析兜底路径下跳过计时)
        baseline_time: float = -1.0
        if ev.get("utils") is None:
            print("  静态分析模式：跳过基线计时（无 PolyBench utilities）")
            print("  只支持 exit_only 正确性验证，无量化加速比")
        else:
            polybench_c = ev["utils"] / "polybench.c"
            _bbin = tempfile.NamedTemporaryFile(delete=False, suffix="_base")
            _bbin.close()
            ok, _ = compile_binary(clang, src, polybench_c, ev["utils"],
                                   ev["source_dir"], Path(_bbin.name))
            baseline_time = tp_run_timing(_bbin.name, runs=args.runs, pin_cpu=pin_cpu)
            try:
                os.unlink(_bbin.name)
            except Exception:
                pass
            if baseline_time <= 0:
                sys.exit("基线计时失败，请检查编译配置")
            print(f"  基线 -O3: {baseline_time:.2f} ms")

        # 快照目录：每步保存修改版本，供回退和审查（存入 run_logger 目录）
        out_dir = _out_dir
        snapshot_dir = out_dir / "snapshots" / name
        snapshot_dir.mkdir(parents=True, exist_ok=True)
        # 保存原始版本作为 step_00_original
        with open(src) as f:
            _orig_text = f.read()
        (snapshot_dir / "step_00_original.c").write_text(_orig_text)
        print(f"  快照目录: {snapshot_dir}")

        best_speedup:  float = 1.0
        best_source:   "str | None" = None
        best_flags:    list = []
        history        = OptimizationHistory()

        # ── 回退栈：每步前保存状态，灾难性退化时自动恢复 ─────────────────────
        # 第4个元素是 utils/polybench.c 的内容快照——rewrite_source 现在能持久化
        # 改写落在 utils 里的热点函数（见 src/hotspot.py + _eval_utils_rewrite），
        # 那次改动是直接写回本次 run 私有的 utils 路径的，不经过 best_source，
        # 所以回退栈要是不快照 utils 内容，灾难性退化回退时驱动文件/flags 复原了，
        # utils 里的坏改动却会一直留着，后续步骤仍然在一个已知变差的 utils 副本上继续。
        rollback_stack: list = []   # List[Tuple[str|None, list, float, str|None]]
        catastrophic_threshold = getattr(
            config.runtime, "catastrophic_slowdown_threshold", 20.0)
        _utils_polybench_c = (Path(ev["utils"]) / "polybench.c"
                              if ev.get("utils") else None)

        # ── 元规划：行动序列缓冲区 ────────────────────────────────────────────
        # plan_action_sequence() 每 3 步（或缓冲区耗尽时）调用一次，返回接下来
        # 最多 3 步的工具序列，保证多工具交替。_anti_repeat_forced() 作为兜底，
        # 防止同一工具连续 ≥2 次。两者合力实现"多次 LLM 参与 + 工具轮换"。
        _planned_seq: list = []   # List[str]  — 来自元规划 LLM 的行动序列缓冲

        for step in range(1, max_steps + 1):
            if _run_logger:
                _current_step[0] = step
            print(f"\n{'─'*60}")
            print(f"[Agent 步骤 {step}/{max_steps}]")

            # ── 决定本步骤的 forced_action ────────────────────────────────────
            _forced: "str | None" = None

            if step == 1:
                # 第1步：强制调参，建立 flags 基线
                _forced = "try_flags"
            elif step == 2:
                # 第2步：强制源码重写，覆盖另一个优化维度
                _forced = "rewrite_source"
            else:
                # 第3步起：先检查反重复（最高优先级），再用计划序列
                _anti = _anti_repeat_forced(history)
                if _anti:
                    # 同一工具已连续 ≥2 次，强制轮换，并让规划器重新规划
                    _forced = _anti
                    _planned_seq.clear()
                else:
                    # 从缓冲区取下一个计划动作；缓冲区空时先调用元规划 LLM
                    if not _planned_seq:
                        _planned_seq = plan_action_sequence(
                            llm, kernel_name, history,
                            step_num=step, max_steps=max_steps,
                            max_tokens=512,
                        )
                    if _planned_seq:
                        _forced = _planned_seq.pop(0)
                        print(f"  [计划序列] 执行规划动作: {_forced}")
                    # 若规划失败（返回空列表），_forced 保持 None，由执行 LLM 自由决策

            # 保存当前最优状态（用于灾难性退化时回退），含 utils/polybench.c 快照
            _utils_snap = (_utils_polybench_c.read_text()
                          if _utils_polybench_c and _utils_polybench_c.exists() else None)
            rollback_stack.append((best_source, list(best_flags), best_speedup, _utils_snap))

            result = run_agent_step(
                src_original=src, config=config, llm=llm,
                ev=ev, runs=args.runs, pin_cpu=pin_cpu,
                epsilon=args.epsilon, max_tokens=args.max_tokens,
                history=history,
                current_best_source=best_source,
                current_best_flags=best_flags,
                step_num=step, max_steps=max_steps,
                baseline_time=baseline_time,
                snapshot_dir=snapshot_dir,
                forced_action=_forced,
            )
            history.record(step, result)

            action = result.get("action", "")
            sp_raw = result.get("speedup", 1.0)
            strat  = result.get("strategy", "")
            err    = result.get("error", "")
            snap   = result.get("snapshot_path", "")

            # ── 灾难性退化检测：自动回退到上一个已知最优状态 ─────────────────
            if (not result.get("error") and action not in ("done", "error") and
                    best_speedup > 1.0 and rollback_stack and
                    sp_raw < best_speedup * (1.0 - catastrophic_threshold / 100.0)):
                prev_src, prev_flags, prev_sp, prev_utils_snap = rollback_stack[-1]
                print(f"  ⚠ 灾难性退化: {sp_raw:.3f}x << 当前最优 {best_speedup:.3f}x "
                      f"(阈值 {catastrophic_threshold:.0f}%)，自动回退到 {prev_sp:.3f}x 状态")
                best_source = prev_src
                best_flags  = prev_flags
                # best_speedup 不变（仍保持历史最优）
                if prev_utils_snap is not None and _utils_polybench_c:
                    _utils_polybench_c.write_text(prev_utils_snap)
                    print(f"  [utils 回退] 已恢复 {_utils_polybench_c} 到回退前状态")

            sp = sp_raw  # 用于后续 if sp > best_speedup 判断

            if action == "done":
                print(f"  LLM 主动终止: {result.get('strategy', result.get('reasoning', ''))}")
                break
            if action == "error":
                print(f"  步骤错误: {err}")
                continue

            perf = result.get("perf_stats", {})
            perf_str = ""
            if perf and not perf.get("error"):
                ipc = perf.get("ipc")
                llc = perf.get("llc_miss_rate")
                if ipc is not None:
                    perf_str = f"  IPC={ipc:.2f}"
                if llc is not None:
                    perf_str += f" LLC_miss={llc:.1f}%"

            if err:
                print(f"  步骤{step}: 失败 [{action}] {err[:300]}")
            else:
                print(f"  步骤{step}: {sp:.3f}x  [{strat}]{perf_str}")
            if snap:
                print(f"  快照: {snap}")

            if sp > best_speedup:
                best_speedup = sp
                new_src = result.get("source")
                if new_src:
                    best_source = new_src
                    # 保存当前最优为 best_checkpoint
                    (out_dir / f"{name}_best_checkpoint.c").write_text(new_src)
                    # 更新 ev["kernel_text"]，让下一步 LLM 看到最新 kernel
                    _new_kernel, _, _ = extract_kernel_function(new_src, ev["kernel_name"])
                    if _new_kernel:
                        ev["kernel_text"] = _new_kernel
                new_flags = result.get("flags", [])
                if new_flags:
                    best_flags = new_flags
                print(f"  *** 新最优: {best_speedup:.3f}x  [{strat}] ***")

        # ── 最终组合测速：若 best_source 和 best_flags 来自不同步骤，补测 ───────
        # `best_source`（来自某次 rewrite_source 步骤）和 `best_flags`（来自某次
        # try_flags 步骤）各自都已经在历史里验证过"比 baseline 更快"，但两者
        # 从未一起测过——组合到一起完全可能互相打架（比如某个 -mllvm 参数是针对
        # 原始源码的 IR 结构调的，重写后 IR 变了，参数反而帮倒忙）。这里补测一次，
        # 但只有测出来真的比单独 source 更好时才采纳组合；测出来更差就必须丢掉
        # flags，回退到已知更优的 source-only 结果——不能让"组合"这个动作本身
        # 覆盖掉已经验证过的更优状态（之前这里没有比较，jacobi-2d 就因为组合测
        # 出 0.9415x 比 source-only 的 1.014x 差，却被当成最终结果报了出来）。
        compound_speedup  = best_speedup
        compound_flags    = best_flags[:]
        compound_verified = False
        if best_source and best_flags and ev.get("utils"):
            # 检查本轮是否已经测过 source+flags 组合
            src_flag_steps = [s for s in history.steps
                              if s.has_source and s.flags and not s.error]
            if not src_flag_steps:
                # 从未同时测过 source+flags，现在测一次
                print("\n[最终组合测速] source + flags 从未同时测过，补测一次...")
                _combo_src = out_dir / f"{name}_compound_check.c"
                _combo_src.write_text(best_source)
                polybench_c = ev["utils"] / "polybench.c"
                with tempfile.TemporaryDirectory() as _ct:
                    _ct = Path(_ct)
                    _cbin = _ct / "compound_time"
                    _ok, _cerr = compile_c(
                        clang, [str(_combo_src), str(polybench_c)],
                        [ev["utils"], ev["source_dir"]],
                        ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"], _cbin,
                        extra_flags=best_flags)
                    if _ok:
                        ct = ts_run_timing(str(_cbin), runs=args.runs, pin_cpu=pin_cpu)
                        if ct > 0 and baseline_time > 0:
                            _combo_sp = baseline_time / ct
                            compound_verified = True
                            if _combo_sp > best_speedup:
                                compound_speedup = _combo_sp
                                compound_flags   = best_flags[:]
                                print(f"  组合加速比: {compound_speedup:.4f}x "
                                      f"({(compound_speedup-1)*100:+.1f}%) ← 比单独 source 更优，保留 flags")
                            else:
                                compound_speedup = best_speedup
                                compound_flags   = []
                                best_flags       = []
                                print(f"  组合测得 {_combo_sp:.4f}x，比单独 source（{best_speedup:.4f}x）更差，"
                                      f"丢弃 flags，最终只保留 source 重写")
                try:
                    _combo_src.unlink()
                except Exception:
                    pass

        # ── 最终确认测量：交替测 baseline/best，配对比值中位数，抑制单次噪声测量
        #    和跨步骤系统负载漂移导致的"挑选偏差"（cherry-picked single sample）──
        confirmation: dict = {"ok": False}
        if ev.get("utils") and (best_source or best_flags) and baseline_time > 0:
            print(f"\n[最终确认] 交替测量 baseline/best 各 {args.runs} 次以降低噪声偏差...")
            polybench_c = ev["utils"] / "polybench.c"
            with tempfile.TemporaryDirectory() as _vt:
                _vt = Path(_vt)
                _vbase_bin = _vt / "confirm_base"
                _ok_b, _ = compile_binary(clang, src, polybench_c, ev["utils"],
                                          ev["source_dir"], _vbase_bin)
                _vbest_src = _vt / f"{name}_confirm.c"
                _vbest_src.write_text(best_source if best_source else _orig_text)
                _vbest_bin = _vt / "confirm_best"
                _ok_c, _cerr = compile_c(
                    clang, [str(_vbest_src), str(polybench_c)],
                    [ev["utils"], ev["source_dir"]],
                    ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"], _vbest_bin,
                    extra_flags=best_flags)
                if _ok_b and _ok_c:
                    confirmation = confirm_result(str(_vbase_bin), str(_vbest_bin),
                                                  args.runs, pin_cpu)
                    if confirmation.get("ok"):
                        print(f"  确认加速比: {confirmation['confirmed_speedup']:.4f}x "
                              f"(IQR [{confirmation['speedup_iqr'][0]:.4f}, "
                              f"{confirmation['speedup_iqr'][1]:.4f}], n={confirmation['n']}, "
                              f"base_cv={confirmation['base_stdev_pct']:.1f}%, "
                              f"best_cv={confirmation['best_stdev_pct']:.1f}%)")
                    else:
                        print("  [warn] 最终确认测量失败（binary 执行异常），沿用单次测量的 best_speedup")
                else:
                    print(f"  [warn] 最终确认编译失败，沿用单次测量的 best_speedup: {_cerr[:200] if _cerr else ''}")

        # ── 最终报告 ──────────────────────────────────────────────────────────
        print("\n" + "=" * 60)
        print(f"程序:            {name}")
        print(f"基线 -O3:        {baseline_time:.2f} ms")
        print(f"已完成步骤:      {len(history.steps)}/{max_steps}")
        print()

        # 参数演化轨迹
        if history.flags_timeline:
            print("参数演化轨迹:")
            for e in history.flags_timeline:
                src_tag = " [+source]" if e["with_source"] else ""
                print(f"  步骤{e['step']:2d} [{e['action']}]{src_tag}: "
                      f"{e['speedup']:.4f}x  {e['flags_str']}")
            print()

        if best_source:
            out_file = out_dir / f"{name}_optimized.c"
            out_file.write_text(best_source)
            print(f"最优源码:        {out_file}")

        if best_flags:
            print(f"最优参数组:      {' '.join(best_flags)}")

        # 优先使用最终确认测量（交替测量+配对中位数，噪声更低），
        # 而不是搜索阶段挑出来的单次测量值——两者可能因噪声差出 50%+，
        # 之前这里一直印的是未确认的 best_speedup，具有误导性。
        reported_speedup = (confirmation.get("confirmed_speedup")
                             if confirmation.get("ok")
                             else (max(compound_speedup, best_speedup) if compound_verified else best_speedup))
        if compound_verified:
            print(f"组合加速比:      {compound_speedup:.4f}x ({(compound_speedup-1)*100:+.1f}%)  [source + flags]")
        print(f"最优加速比:      {reported_speedup:.4f}x ({(reported_speedup-1)*100:+.1f}%)"
              + ("  [已用最终确认测量校正]" if confirmation.get("ok") and abs(reported_speedup - best_speedup) > 1e-6 else ""))

        print()
        if best_source and best_flags:
            print(f"编译命令:  clang -O3 {' '.join(best_flags)} {out_file} ...")
        elif best_source:
            print(f"编译命令:  clang -O3 {out_file} ...")
        elif best_flags:
            print(f"编译命令:  clang -O3 {' '.join(best_flags)} {src} ...")
        else:
            print("未找到有效优化（无源码改进，无有效参数）。")

        print(f"快照目录:        {snapshot_dir}")

        json_out = out_dir / f"{name}_agent_results.json"
        with open(json_out, "w") as f:
            json.dump({
                "program":            name,
                "baseline_ms":        baseline_time,
                "max_steps":          max_steps,
                "steps_taken":        len(history.steps),
                "best_speedup":       best_speedup,
                "compound_speedup":   compound_speedup,
                "confirmed_speedup":  confirmation.get("confirmed_speedup") if confirmation.get("ok") else None,
                "confirmation":       confirmation if confirmation.get("ok") else None,
                "best_flags":         best_flags,
                "has_source_rewrite": best_source is not None,
                "flags_timeline":     history.flags_timeline,
                "history":            history.to_dict(),
            }, f, indent=2, ensure_ascii=False)
        print(f"结果 JSON:       {json_out}")

        pg = ev.get("pass_graph", {})
        if pg.get("dot_path"):
            print(f"Pass graph:   {pg['dot_path']}")
        if pg.get("png_path") and Path(pg["png_path"]).exists():
            print(f"Pass PNG:     {pg['png_path']}")
        print("=" * 60)

        # ── 保存最终结果到 RunLogger ──────────────────────────────────────────
        if _run_logger:
            try:
                _run_logger.save_results({
                    "program":          name,
                    "dataset":          dataset_type,
                    "baseline_ms":      baseline_time,
                    "best_speedup":     (confirmation.get("confirmed_speedup")
                                          if confirmation.get("ok")
                                          else (max(compound_speedup, best_speedup) if compound_verified else best_speedup)),
                    "best_flags":       best_flags,
                    "steps_taken":      len(history.steps),
                    "flags_timeline":   history.flags_timeline,
                })
                if best_source:
                    _run_logger.snapshot(f"{name}_optimized.c", best_source)
            except Exception as _save_err:
                print(f"[warn] 日志保存失败: {_save_err}")
            finally:
                _run_logger.close()
        return

    # ── Legacy mode (--param-only / --source-only) ────────────────────────────
    utils      = find_polybench_utilities(src)
    source_dir = Path(src).resolve().parent

    vect_remarks   = []
    cpu_cache_info = ""
    if utils and not args.param_only:
        print("Collecting vectorization remarks for source rounds...")
        vect_remarks   = extract_vectorization_remarks(clang, src, utils, source_dir)
        cpu_cache_info = ts_get_cache_info()
        print(f"  {len(vect_remarks)} remarks collected")

    param_result      = {}
    param_extra_flags = []
    param_analysis    = ""
    if not args.source_only:
        print("\n[Param round]")
        param_result = run_param_round(src, config, llm, args.runs, pin_cpu, args.top_k)
        param_extra_flags = param_result.get("best_flags_list", [])
        param_analysis    = param_result.get("analysis", "")
        if param_extra_flags:
            print(f"  Param flags for source rounds: {' '.join(param_extra_flags)}")
    if args.param_only:
        return

    orig_kernel_name  = kernel_name
    best_speedup      = 1.0
    best_source       = None
    best_retune_flags = param_extra_flags[:]
    best_retune_str   = param_result.get("best_flags", "")
    current_rewrite   = src
    round_tmp_files: list = []
    prev_error        = None

    def _analyze(kernel_src: str, remarks: list) -> str:
        print("  Analyzing kernel optimization patterns...")
        analysis = analyze_kernel_patterns(llm, kernel_src, remarks, max_tokens=12800)
        if analysis:
            print(f"  Analysis preview: {analysis[:200].splitlines()[0]}...")
        else:
            print("  (analysis LLM returned nothing)")
        return analysis

    def _retune(source_text: str) -> tuple:
        fd, tmp_name = tempfile.mkstemp(prefix=f"{name}_retune_", suffix=".c")
        os.close(fd)
        tmp = Path(tmp_name)
        try:
            tmp.write_text(source_text)
            res = run_param_round(str(tmp), config, llm, args.runs, pin_cpu, args.top_k,
                                  kernel_name=orig_kernel_name)
            flags = res.get("best_flags_list", [])
            fstr  = res.get("best_flags", "")
            if flags:
                print(f"  Re-tuned flags: {' '.join(flags)}")
            else:
                flags = param_extra_flags[:]
                fstr  = " ".join(flags)
            return flags, fstr
        except Exception as e:
            print(f"  Re-tuning failed: {e}")
            return param_extra_flags[:], " ".join(param_extra_flags)
        finally:
            try: tmp.unlink()
            except Exception: pass

    with open(src) as _f:
        _orig_text = _f.read()
    _orig_kernel, _, _ = extract_kernel_function(_orig_text, orig_kernel_name)
    kernel_analysis = _analyze(_orig_kernel or _orig_text, vect_remarks)
    del _orig_text, _orig_kernel

    rich_remarks: dict = {}
    if utils and not args.param_only:
        try:
            print("Extracting rich YAML remarks for source rounds...")
            rich_remarks = _extract_rich_remarks_src(
                clang, src, utils, source_dir, orig_kernel_name)
            print(f"  Rich remarks passes: {list(rich_remarks.keys())}")
        except Exception as _e:
            print(f"  Rich remarks extraction failed ({_e}), continuing without.")

    precision_failures: list = []
    attempt_history:   list = []

    source_rounds = max_steps - (0 if args.source_only else 1)
    try:
        for idx in range(1, source_rounds + 1):
            print(f"\n[Source round {idx}/{source_rounds}]"
                  + (f"  (building on round {idx-1} best)" if current_rewrite != src else ""))

            retry_error = prev_error
            result = {"status": "no_response", "error": "not run", "speedup": 1.0}
            for retry in range(1, 4):
                if retry > 1:
                    print(f"  [Retry {retry}/3]")
                result = run_source_round(
                    rewrite_src=current_rewrite,
                    ref_src=src,
                    config=config, llm=llm,
                    runs=args.runs, pin_cpu=pin_cpu,
                    epsilon=args.epsilon, max_tokens=args.max_tokens,
                    vect_remarks=vect_remarks, cpu_cache_info=cpu_cache_info,
                    prev_error=retry_error, attempt=idx,
                    param_extra_flags=param_extra_flags,
                    param_analysis=param_analysis,
                    extra_hints=(f"### Kernel pattern analysis\n{kernel_analysis}\n"
                                 if kernel_analysis else ""),
                    rich_remarks=rich_remarks or None,
                    precision_failures=precision_failures,
                    attempt_history=attempt_history or None,
                )
                if result.get("status") == "ok":
                    break
                retry_error = result.get("error", "")
                print(f"  Attempt {retry} failed ({result.get('status')}): "
                      f"{retry_error[:120]}")

            # Record this round's outcome in attempt history
            attempt_history.append({
                "attempt": idx,
                "status":  result.get("status", "?"),
                "speedup": result.get("speedup", 1.0),
                "error":   result.get("error", "")[:200],
            })

            status = result.get("status")
            if status == "ok":
                sp = result["speedup"]
                ms = result["time_ms"]
                print(f"  Speedup: {sp:.3f}x ({(sp-1)*100:+.1f}%)  {ms:.2f} ms")
                if sp < 0.98:
                    prev_error = (
                        f"PERFORMANCE REGRESSION: last rewrite ran at {sp:.3f}x "
                        f"({ms:.1f} ms vs baseline {result.get('orig_time_ms', 0):.1f} ms)\n"
                        "Try a DIFFERENT strategy."
                    )
                else:
                    prev_error = None

                if sp > best_speedup:
                    best_speedup = sp
                    best_source  = result["source"]
                    print(f"  → New best ({sp:.3f}x)!")
                    fd, _tmp_name = tempfile.mkstemp(prefix=f"{name}_round{idx}_", suffix=".c")
                    os.close(fd)
                    tmp_path = Path(_tmp_name)
                    tmp_path.write_text(best_source)
                    round_tmp_files.append(tmp_path)
                    current_rewrite = str(tmp_path)
                    new_remarks = extract_vectorization_remarks(
                        clang, current_rewrite, utils, source_dir)
                    if new_remarks:
                        vect_remarks = new_remarks
                    new_kernel, _, _ = extract_kernel_function(best_source, orig_kernel_name)
                    kernel_analysis = _analyze(new_kernel or best_source, vect_remarks)
                    best_retune_flags, best_retune_str = _retune(best_source)
                else:
                    print(f"  No improvement over best ({best_speedup:.3f}x)")
            else:
                print(f"  Round {idx} failed ({status}): {result.get('error','')[:200]}")
                prev_error = result.get("error", "")
    finally:
        for f in round_tmp_files:
            try: f.unlink()
            except Exception: pass

    _print_legacy_summary(name, param_result, best_source, best_speedup,
                          best_retune_str, best_retune_flags)


def _print_legacy_summary(name: str, param_result: dict, best_source: "str | None",
                          best_speedup: float, best_retune_str: str,
                          best_retune_flags: list) -> None:
    """Print the final summary for the legacy --param-only/--source-only path."""
    print("\n" + "=" * 60)
    print(f"Program:      {name}")
    if param_result.get("baseline_ms"):
        print(f"Baseline -O3: {param_result['baseline_ms']:.2f} ms")
    if param_result.get("best_flags"):
        print(f"Param flags:  {param_result['best_flags']}  ({param_result['best_speedup']:.3f}x)")
    if best_source:
        print(f"Source best:  {best_speedup:.3f}x")
        if best_retune_str:
            print(f"Retune flags: {best_retune_str}")
        out_dir  = Path("outputs"); out_dir.mkdir(exist_ok=True)
        out_file = out_dir / f"{name}_optimized.c"
        with open(out_file, "w") as f:
            f.write(best_source)
        print(f"Saved:        {out_file}")
        if best_retune_flags:
            print(f"To compile:   clang -O3 "
                  f"{' '.join(best_retune_flags)} {out_file} ...")
    else:
        print("Source:       no verified improvement")
    print("=" * 60)


if __name__ == "__main__":
    main()
