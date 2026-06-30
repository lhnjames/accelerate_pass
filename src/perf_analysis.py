"""
perf_analysis.py — 使用 perf stat 收集硬件性能计数器，分类瓶颈类型，
并将瓶颈反向映射到 O3 pass 参数。

瓶颈分类 → O3 Pass 映射（逆向思维）：
  内存瓶颈(LLC miss高, IPC低)    → LoopInterchange, LICM(hoist), LoopDistribute
  向量化不足(IPC低, SIMD未开启)  → SLPVectorizer(threshold), LoopVectorize
  循环开销大(分支/循环control重) → LoopUnroll(threshold), SimplifyCFG
  内联不足(调用开销)              → Inliner(threshold)
  冗余计算(GVN missed)           → GVN(max-hoisted, hoist-max-bbs)
"""
import re
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple, Optional

# ── Per-pass rationale (shown to LLM; explains what each pass does) ──────────
# Parameters are discovered dynamically from `opt --help-hidden` at runtime —
# never hardcoded here.  This dict only records the human-readable explanation
# and the bottleneck types each pass is relevant to.
_PASS_META: Dict[str, Tuple[List[str], str]] = {
    "SLPVectorizerPass":         (["low_ipc", "vectorization_gap"],
        "SLP向量化；slp-threshold负值扩大cost窗口，schedule-budget增大允许更复杂依赖图"),
    "LoopVectorizePass":         (["low_ipc", "vectorization_gap"],
        "Loop向量化；min-trip-count降低让短循环也向量化；scev-check-threshold控制运行时检查数"),
    "GVN":                       (["redundant_compute", "memory_bound"],
        "全局值编号；gvn-max-hoisted/hoist-max-bbs增大允许把更多冗余load提升出循环"),
    "LICMPass":                  (["memory_bound", "redundant_compute"],
        "循环不变代码外提；licm-max-num-uses-traversed增大让更多不变量被识别并移出循环"),
    "LoopUnrollPass":            (["branch_overhead", "low_ipc", "memory_bound", "vectorization_gap"],
        "循环展开；unroll-threshold增大允许展开更多迭代，减少控制开销，扩大向量化宽度"),
    "LoopFullUnrollPass":        (["branch_overhead", "low_ipc"],
        "完全展开；unroll-full-max-count控制完全展开的最大迭代次数"),
    "SimpleLoopUnswitchPass":    (["branch_overhead", "low_ipc"],
        "循环外提条件分支；unswitch-threshold增大让含分支的循环也被unswitched"),
    "InlinerPass":               (["call_overhead"],
        "内联；inline-threshold增大允许更激进内联，消除调用开销"),
    "DSEPass":                   (["redundant_compute", "memory_bound"],
        "死存储消除；dse-memoryssa-scanlimit增大允许更深的内存依赖分析"),
    "MemCpyOptPass":             (["memory_bound"],
        "memcpy/memset聚合；max-store-memcpy增大让更多连续store聚合为单次memcpy"),
    "EarlyCSEPass":              (["redundant_compute"],
        "早期公共子表达式消除；earlycse-mssa-optimization-cap增大允许更深的MemorySSA分析"),
    "InstCombinePass":           (["redundant_compute", "low_ipc"],
        "指令合并化简；instcombine-max-iterations增大允许更多轮代数化简"),
    "AggressiveInstCombinePass": (["redundant_compute", "low_ipc"],
        "激进指令合并；使用与InstCombine相同的阈值参数，但尝试更复杂的等价变换"),
    "JumpThreadingPass":         (["branch_overhead"],
        "跳转线程化；jump-threading-threshold增大允许更多条件跳转被线程化"),
    "SpeculativeExecutionPass":  (["branch_overhead", "low_ipc"],
        "推测执行提升；spec-exec-max-speculation-cost增大允许更激进的推测"),
    "CallSiteSplittingPass":     (["branch_overhead", "call_overhead"],
        "调用点拆分；callsite-splitting-duplication-threshold增大允许更多代码复制"),
    "SimplifyCFGPass":           (["branch_overhead"],
        "控制流简化；simplifycfg-max-small-block-size增大允许合并更大的基本块"),
    "LoopDistributePass":        (["vectorization_gap", "memory_bound"],
        "循环分布；loop-distribute-scev-check-threshold增大允许更多运行时别名检查"),
    "LoopLoadEliminationPass":   (["memory_bound", "redundant_compute"],
        "循环load消除；loop-load-elimination-scev-check-threshold增大允许更多运行时检查"),
    "Float2IntPass":             (["low_ipc"],
        "浮点→整数转换；float2int-max-integer-bw控制最大整数位宽，整数路径通常更快"),
    # Passes with no tunable -mllvm params — guidance is for source-level rewrites
    "LoopInterchangePass":       (["memory_bound", "cache_inefficiency"],
        "循环交换改善空间局部性；无-mllvm参数，需源码重写（交换循环嵌套顺序）"),
    "SROAPass":                  (["vectorization_gap", "redundant_compute"],
        "标量替换；无-mllvm参数，但源码用局部累加器代替数组下标可辅助SROA促进向量化"),
    "SCCPPass":                  (["redundant_compute", "branch_overhead"],
        "常量传播；无-mllvm参数，源码中显式常量有助于SCCP消除死分支"),
    "IndVarSimplifyPass":        (["vectorization_gap", "low_ipc"],
        "归纳变量化简；无-mllvm参数，清晰的循环边界有助于SCEV分析和向量化"),
    "ReassociatePass":           (["low_ipc", "vectorization_gap"],
        "表达式重关联；无-mllvm参数，-ffast-math启用后效果更强"),
    "VectorCombinePass":         (["vectorization_gap", "low_ipc"],
        "向量指令合并；无-mllvm参数，源码保持规则的SIMD宽度有助于此pass"),
}

# 瓶颈类型 → 优先检查的 pass 集合
_BOTTLENECK_PASS_PRIORITY: Dict[str, List[str]] = {
    "memory_bound":       ["LICMPass", "GVN", "LoopUnrollPass", "SLPVectorizerPass",
                           "LoopInterchangePass", "MemCpyOptPass", "DSEPass",
                           "LoopDistributePass", "LoopLoadEliminationPass"],
    "vectorization_gap":  ["SLPVectorizerPass", "LoopVectorizePass", "LoopUnrollPass",
                           "LoopDistributePass", "SROAPass", "IndVarSimplifyPass"],
    "low_ipc":            ["SLPVectorizerPass", "LoopVectorizePass", "LoopUnrollPass",
                           "GVN", "InstCombinePass", "ReassociatePass",
                           "SpeculativeExecutionPass", "SimpleLoopUnswitchPass"],
    "redundant_compute":  ["GVN", "EarlyCSEPass", "LICMPass", "DSEPass",
                           "InstCombinePass", "SCCPPass", "SROAPass"],
    "branch_overhead":    ["LoopUnrollPass", "SimpleLoopUnswitchPass", "JumpThreadingPass",
                           "SimplifyCFGPass", "SpeculativeExecutionPass",
                           "CallSiteSplittingPass", "InlinerPass"],
    "call_overhead":      ["InlinerPass", "CallSiteSplittingPass"],
    "cache_inefficiency": ["LoopInterchangePass", "LICMPass", "GVN", "LoopUnrollPass",
                           "LoopDistributePass"],
    "unknown":            ["LICMPass", "SLPVectorizerPass", "GVN", "LoopUnrollPass",
                           "InstCombinePass", "EarlyCSEPass"],
}


def collect_perf_stats(binary_path: str, runs: int = 3,
                       pin_cpu: Optional[int] = None) -> dict:
    """
    用 perf stat 收集硬件计数器。
    返回：{ipc, llc_miss_rate, l1_miss_rate, cycles, instructions,
            cache_misses, wall_time_s, cpu_util_pct, bottleneck_hints}
    """
    prefix = []
    if pin_cpu is not None:
        prefix = ["taskset", "-c", str(pin_cpu)]

    events = (
        "cycles,instructions,"
        "cache-misses,cache-references,"
        "L1-dcache-load-misses,L1-dcache-loads,"
        "branch-misses,branch-instructions"
    )
    cmd = prefix + [
        "perf", "stat",
        "-e", events,
        "-r", str(runs),
        "--", binary_path,
    ]

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=120
        )
        raw = result.stderr  # perf stat writes to stderr
    except Exception as e:
        return {"error": str(e), "bottleneck_hints": []}

    stats = _parse_perf_stat(raw)
    stats["bottleneck_hints"] = _classify_bottleneck(stats)
    return stats


def _parse_perf_stat(raw: str) -> dict:
    """从 perf stat stderr 中提取数值。"""
    def _num(pattern):
        m = re.search(pattern, raw, re.MULTILINE)
        if not m:
            return None
        s = m.group(1).replace(",", "").replace(".", "")
        try:
            return int(s)
        except ValueError:
            return None

    def _float(pattern):
        m = re.search(pattern, raw, re.MULTILINE)
        if not m:
            return None
        try:
            return float(m.group(1).replace(",", "."))
        except ValueError:
            return None

    cycles      = _num(r"^\s*([\d,]+)\s+cycles")
    instrs      = _num(r"^\s*([\d,]+)\s+instructions")
    cache_miss  = _num(r"^\s*([\d,]+)\s+cache-misses")
    cache_ref   = _num(r"^\s*([\d,]+)\s+cache-references")
    l1_miss     = _num(r"^\s*([\d,]+)\s+L1-dcache-load-misses")
    l1_load     = _num(r"^\s*([\d,]+)\s+L1-dcache-loads")
    br_miss     = _num(r"^\s*([\d,]+)\s+branch-misses")
    br_total    = _num(r"^\s*([\d,]+)\s+branch-instructions")
    wall_time   = _float(r"([\d.]+)\s+seconds time elapsed")
    user_time   = _float(r"([\d.]+)\s+seconds user")

    ipc = (instrs / cycles) if cycles and instrs and cycles > 0 else None
    llc_miss_rate = (cache_miss / cache_ref * 100
                     if cache_miss is not None and cache_ref and cache_ref > 0
                     else None)
    l1_miss_rate  = (l1_miss / l1_load * 100
                     if l1_miss is not None and l1_load and l1_load > 0
                     else None)
    br_miss_rate  = (br_miss / br_total * 100
                     if br_miss is not None and br_total and br_total > 0
                     else None)
    cpu_util      = (user_time / wall_time * 100
                     if user_time is not None and wall_time and wall_time > 0
                     else None)

    return {
        "cycles":        cycles,
        "instructions":  instrs,
        "ipc":           round(ipc, 3)           if ipc           is not None else None,
        "llc_miss_rate": round(llc_miss_rate, 1) if llc_miss_rate is not None else None,
        "l1_miss_rate":  round(l1_miss_rate, 1)  if l1_miss_rate  is not None else None,
        "br_miss_rate":  round(br_miss_rate, 1)  if br_miss_rate  is not None else None,
        "cpu_util_pct":  round(cpu_util, 1)      if cpu_util      is not None else None,
        "wall_time_s":   wall_time,
    }


def _classify_bottleneck(stats: dict) -> List[str]:
    """
    根据硬件计数器推断瓶颈类型（可多个）。
    优先使用 VTune Top-Down memory_bound_pct（更准确），其次 perf 启发式。
    返回有序列表，越靠前优先级越高。
    """
    hints = []
    ipc          = stats.get("ipc")
    llc_miss     = stats.get("llc_miss_rate")
    l1_miss      = stats.get("l1_miss_rate")
    br_miss      = stats.get("br_miss_rate")
    cpu_util     = stats.get("cpu_util_pct")
    vtune_membnd = stats.get("vtune_memory_bound_pct")  # VTune Top-Down (authoritative)

    # VTune Top-Down memory bound（优先于 perf 启发式）
    if vtune_membnd is not None:
        if vtune_membnd > 50:
            hints.append("memory_bound")     # 高置信：超过50%时间等待内存
        elif vtune_membnd > 25:
            hints.append("memory_bound")     # 中置信：内存瓶颈明显
        # 25% 以下不加，避免过度分类
    else:
        # 无 VTune 数据时使用 perf 启发式
        if llc_miss is not None and llc_miss > 10:
            hints.append("memory_bound")

    # L1 cache 效率问题（perf，与 VTune 互补）
    if l1_miss is not None and l1_miss > 20:
        hints.append("cache_inefficiency")

    # 向量化/计算瓶颈：IPC低但不是内存瓶颈
    if ipc is not None:
        if ipc < 0.5 and "memory_bound" not in hints:
            hints.append("low_ipc")
        if 0.5 <= ipc < 1.5 and llc_miss is not None and llc_miss < 5:
            hints.append("vectorization_gap")  # 计算能力未充分利用

    # 分支预测差
    if br_miss is not None and br_miss > 5:
        hints.append("branch_overhead")

    # CPU利用率低（无 VTune 时的内存等待代理）
    if cpu_util is not None and cpu_util < 70:
        if "memory_bound" not in hints:
            hints.append("memory_bound")

    if not hints:
        hints.append("unknown")

    return hints


def format_perf_summary(stats: dict) -> str:
    """
    生成给 LLM 看的 perf 摘要（中英文双语，英文数值）。
    """
    if "error" in stats:
        return f"  perf stat failed: {stats['error']}"

    lines = []
    ipc = stats.get("ipc")
    if ipc is not None:
        quality = "良好" if ipc > 2.0 else ("中等" if ipc > 1.0 else "很低(内存等待或串行依赖)")
        lines.append(f"  IPC (instructions/cycle): {ipc:.2f}  [{quality}]")

    llc = stats.get("llc_miss_rate")
    if llc is not None:
        quality = "严重" if llc > 20 else ("偏高" if llc > 5 else "正常")
        lines.append(f"  LLC miss rate: {llc:.1f}%  [{quality}]")

    l1 = stats.get("l1_miss_rate")
    if l1 is not None:
        lines.append(f"  L1-dcache miss rate: {l1:.1f}%")

    br = stats.get("br_miss_rate")
    if br is not None and br > 1:
        lines.append(f"  Branch miss rate: {br:.1f}%")

    cpu = stats.get("cpu_util_pct")
    if cpu is not None:
        quality = "正常" if cpu > 90 else ("偏低" if cpu > 60 else "很低(内存bound或IO等待)")
        lines.append(f"  CPU utilization: {cpu:.1f}%  [{quality}]")

    hints = stats.get("bottleneck_hints", ["unknown"])
    lines.append(f"  瓶颈推断: {', '.join(hints)}")

    # VTune 补充数据（若已采集）
    vtune_mb = stats.get("vtune_memory_bound_pct")
    if vtune_mb is not None:
        quality = ("严重(>50%)" if vtune_mb > 50
                   else "中等(25-50%)" if vtune_mb > 25
                   else "轻度(<25%)")
        lines.append(f"  Memory bound (VTune Top-Down): {vtune_mb:.1f}%  [{quality}]")
    vtune_vec = stats.get("vtune_vectorized_time_pct")
    if vtune_vec is not None:
        lines.append(f"  Vectorized time (VTune): {vtune_vec:.1f}%")
    vtune_hotspots = stats.get("vtune_top_hotspots", [])
    if vtune_hotspots:
        lines.append(f"  VTune top hotspot: {vtune_hotspots[0]['func'][:40]} "
                     f"({vtune_hotspots[0]['cpu_time_pct']:.1f}% CPU)")
    vtune_err = stats.get("vtune_error")
    if vtune_err:
        lines.append(f"  VTune: {vtune_err}")

    return "\n".join(lines)


def get_targeted_passes(bottleneck_hints: List[str],
                        kernel_passes: List[str],
                        kernel_remarks: dict,
                        discovered_opts: Optional[dict] = None) -> List[dict]:
    """
    逆向映射：根据瓶颈类型 + 实际跑过的 O3 pass + missed remarks + 动态发现的参数
    返回最应该调整的 pass 列表，按优先级排序。

    参数 (params) 来自 discovered_opts (runtime opt --help-hidden 发现) —— 无硬编码。
    若 discovered_opts 为 None，则 params 字段为空（仅做优先级排序，实际参数另行查询）。

    返回：[{pass_name, params, bottleneck, rationale, missed_count, has_params}]
    """
    if discovered_opts is None:
        discovered_opts = {}

    missed_counts: Dict[str, int] = {}
    for pname, entries in kernel_remarks.items():
        mc = sum(1 for e in entries if e.get("type") == "missed")
        if mc > 0:
            missed_counts[pname] = mc

    # 按瓶颈类型 + 实际运行过的 pass 确定优先顺序
    priority_order: List[str] = []
    seen: set = set()
    for hint in bottleneck_hints:
        for p in _BOTTLENECK_PASS_PRIORITY.get(hint, []):
            if p not in seen:
                priority_order.append(p)
                seen.add(p)
    # 把有 missed remarks 但不在 priority_order 里的 pass 追加到末尾
    for p in missed_counts:
        if p not in seen:
            priority_order.append(p)
            seen.add(p)

    results = []
    seen_passes: set = set()
    for pass_name in priority_order:
        if pass_name in seen_passes:
            continue
        seen_passes.add(pass_name)

        if pass_name not in kernel_passes and pass_name not in kernel_remarks:
            continue

        # Parameters from runtime discovery (no hardcoding)
        params = [o["flag"] for o in discovered_opts.get(pass_name, [])]
        mc = missed_counts.get(pass_name, 0)

        # Skip passes with neither missed remarks nor discoverable params
        if mc == 0 and not params:
            continue

        meta = _PASS_META.get(pass_name, (bottleneck_hints, f"{pass_name} — O3 pipeline pass"))
        applicable_types, rationale = meta
        matched_types = [h for h in bottleneck_hints if h in applicable_types]

        results.append({
            "pass_name":    pass_name,
            "params":       params,
            "bottleneck":   matched_types[0] if matched_types else bottleneck_hints[0],
            "rationale":    rationale,
            "missed_count": mc,
            "has_params":   bool(params),
        })

    return results


def collect_combined_profile(binary_path: str,
                             vtune_enabled: bool = False,
                             vtune_result_dir: str = None,
                             runs: int = 3,
                             pin_cpu: Optional[int] = None) -> dict:
    """
    Collect hardware profile using perf stat (always) and optionally VTune.
    Returns a unified dict merging both sources, then classifies bottlenecks.
    VTune memory_bound_pct takes priority over perf heuristics when present.
    """
    import tempfile as _tf
    stats = collect_perf_stats(binary_path, runs=runs, pin_cpu=pin_cpu)

    if vtune_enabled:
        try:
            from src.vtune_analysis import is_vtune_available, collect_vtune_stats
            if is_vtune_available():
                _vtdir = vtune_result_dir or _tf.mkdtemp(prefix="vtune_")
                vt = collect_vtune_stats(binary_path, _vtdir)
                if not vt.get("error"):
                    # Merge VTune data — keys don't conflict with perf keys
                    stats["vtune_memory_bound_pct"] = vt.get("memory_bound_pct")
                    stats["vtune_vectorized_time_pct"] = vt.get("vectorized_time_pct")
                    stats["vtune_top_hotspots"] = vt.get("top_hotspots", [])
                    stats["vtune_error"] = None
                else:
                    stats["vtune_error"] = vt.get("error")
            else:
                stats["vtune_error"] = "vtune binary not found or not functional"
        except Exception as _e:
            stats["vtune_error"] = str(_e)

    # Reclassify bottleneck with VTune data when available
    stats["bottleneck_hints"] = _classify_bottleneck(stats)
    return stats


def format_targeted_passes(targeted: List[dict], discovered_opts: dict) -> str:
    """生成给 LLM 的定向 pass 分析文本。
    params 直接来自 discovered_opts (runtime 发现)，无需二次查找。
    """
    if not targeted:
        return "  未找到与当前瓶颈匹配的可调 O3 pass。"

    lines = []
    for item in targeted:
        pname  = item["pass_name"]
        mc     = item["missed_count"]
        hint   = item["bottleneck"]
        rat    = item["rationale"]
        params = item["params"]   # already resolved flag names from discovered_opts

        lines.append(f"\n**{pname}**")
        lines.append(f"  瓶颈关联: {hint}  |  missed remarks: {mc}次")
        lines.append(f"  作用: {rat}")

        # Show full flag info from discovered_opts
        opts_for_pass = discovered_opts.get(pname, [])
        if opts_for_pass:
            lines.append("  可调参数 (opt --help-hidden 动态发现):")
            for o in opts_for_pass:
                lines.append(f"    {o['flag']}=<{o['type']}>  -- {o['desc']}")
        elif params:
            # params present but not in discovered_opts (shouldn't happen normally)
            for p in params:
                lines.append(f"    {p}")
        else:
            lines.append("  无直接-mllvm参数，需通过源码重写改善（循环交换、tiling 等）")

    return "\n".join(lines)
