"""
static_optimizer.py — Source/IR-based optimization hints without runtime data.

Used as a fallback when PolyBench utilities are missing (exit_only mode),
or as supplementary analysis when perf data is unavailable. Detects loop
patterns and access strides from source/IR to infer likely bottlenecks.

This is NOT a replacement for hardware profiling — it provides structured
hypotheses for the LLM to reason about in absence of measured data.
"""
import re
from typing import List, Tuple, Dict, Optional


# ── Source pattern analysis ───────────────────────────────────────────────────

def analyze_source_patterns(source_code: str) -> dict:
    """
    Regex-based detection of loop and memory access patterns in C source.

    Returns:
      loop_depth:       int   — max for-loop nesting depth found
      innermost_stride: str   — "row_major", "column_major", or "unknown"
      has_reduction:    bool  — "+= / *= in innermost loop
      has_stencil:      bool  — A[i+k][j] or A[i-1][j] style stencil access
      has_triangular:   bool  — k < i or j < k loop bounds (triangular)
      array_count:      int   — distinct array names accessed
      innermost_loop_var: str — variable name of innermost loop (heuristic)
      loop_vars:        list  — detected loop variable names
      has_matmul_pattern: bool — triple-nested loop with += accumulation
    """
    lines = source_code.splitlines()

    # ── Loop depth ────────────────────────────────────────────────────────────
    max_depth = 0
    current_depth = 0
    loop_vars: List[str] = []
    loop_var_stack: List[str] = []
    for line in lines:
        stripped = line.strip()
        m = re.match(r'for\s*\(\s*(?:int\s+)?(\w+)\s*=', stripped)
        if m:
            current_depth += 1
            max_depth = max(max_depth, current_depth)
            var = m.group(1)
            loop_var_stack.append(var)
            if var not in loop_vars:
                loop_vars.append(var)
        brace_open  = stripped.count("{")
        brace_close = stripped.count("}")
        # Heuristic: closing braces reduce depth
        current_depth = max(0, current_depth - brace_close + brace_open)
        # Note: this heuristic is approximate; single-statement for loops
        # without braces are not handled perfectly.

    # ── Innermost loop variable ───────────────────────────────────────────────
    innermost_var = loop_var_stack[-1] if loop_var_stack else ""

    # ── Array access stride detection ────────────────────────────────────────
    # Look for patterns like A[i][j] vs A[j][i] in innermost context
    # "row_major" = innermost var is the last subscript (good locality)
    # "column_major" = innermost var is first subscript (bad locality for C)
    innermost_stride = "unknown"
    if innermost_var:
        # Find all array accesses: name[...][...]
        array_accesses = re.findall(r'\w+\[([^\]]+)\]\[([^\]]+)\]', source_code)
        if array_accesses:
            row_major_count = sum(
                1 for (outer, inner) in array_accesses
                if innermost_var in inner and innermost_var not in outer
            )
            col_major_count = sum(
                1 for (outer, inner) in array_accesses
                if innermost_var in outer and innermost_var not in inner
            )
            if row_major_count > col_major_count:
                innermost_stride = "row_major"
            elif col_major_count > row_major_count:
                innermost_stride = "column_major"

    # ── Reduction pattern: += or *= in innermost body ─────────────────────────
    has_reduction = bool(re.search(r'\w+\s*[+\-*\/]=', source_code))

    # ── Stencil: accesses like A[i+1][j] or A[i-1][j] ────────────────────────
    has_stencil = bool(re.search(
        r'\w+\[\w+[+-]\d+\]\[?\w*\]?', source_code))

    # ── Triangular loop bounds: k < i, j < k, etc. ───────────────────────────
    has_triangular = bool(re.search(
        r'for\s*\([^;]+;\s*\w+\s*<\s*\w+\s*;', source_code))

    # ── Array count ───────────────────────────────────────────────────────────
    # Find all distinct uppercase (PolyBench style) and lowercase array names
    array_names = set(re.findall(r'\b([A-Za-z]\w*)\s*\[', source_code))
    # Filter out loop variables and common non-array names
    array_names -= set(loop_vars)
    array_names -= {"for", "if", "while", "int", "double", "float"}
    array_count = len(array_names)

    # ── Matrix multiply pattern: i-k-j triple loop with += ────────────────────
    has_matmul_pattern = (max_depth >= 3 and has_reduction
                          and len(loop_vars) >= 3
                          and innermost_stride in ("row_major", "unknown"))

    return {
        "loop_depth":          max_depth,
        "innermost_stride":    innermost_stride,
        "innermost_loop_var":  innermost_var,
        "loop_vars":           loop_vars,
        "has_reduction":       has_reduction,
        "has_stencil":         has_stencil,
        "has_triangular":      has_triangular,
        "array_count":         array_count,
        "has_matmul_pattern":  has_matmul_pattern,
    }


def analyze_ir_patterns(ir_text: str) -> dict:
    """
    Extract loop/memory structure from LLVM O0 IR text.

    Returns:
      phi_count:        int  — number of phi nodes (proxy for loop count)
      gep_count:        int  — getelementptr instructions (memory access density)
      has_vector_types: bool — any <N x type> present (vectorization potential)
      load_count:       int
      store_count:      int
      total_instr:      int
    """
    lines = ir_text.splitlines()
    phi_count   = sum(1 for l in lines if re.match(r"\s+%\S+\s*=\s*phi", l))
    gep_count   = sum(1 for l in lines
                      if re.match(r"\s+%\S+\s*=\s*getelementptr", l))
    load_count  = sum(1 for l in lines if re.match(r"\s+%\S+\s*=\s*load", l))
    store_count = sum(1 for l in lines if re.match(r"\s+store\b", l))
    has_vector  = any(re.search(r"<\d+ x ", l) for l in lines)
    total_instr = sum(1 for l in lines
                      if re.match(r"\s+(%\S+\s*=|store|call|ret|br)", l))
    return {
        "phi_count":        phi_count,
        "gep_count":        gep_count,
        "load_count":       load_count,
        "store_count":      store_count,
        "has_vector_types": has_vector,
        "total_instr":      total_instr,
    }


# ── Bottleneck inference rules ────────────────────────────────────────────────

def infer_bottleneck_without_data(patterns: dict,
                                  ir_patterns: dict) -> List[Tuple[str, float, str]]:
    """
    Rule-based bottleneck inference from static patterns.

    Returns list of (bottleneck_type, confidence, rationale) sorted by confidence desc.
    confidence is in [0.0, 1.0].
    """
    inferred: List[Tuple[str, float, str]] = []
    depth    = patterns.get("loop_depth", 0)
    stride   = patterns.get("innermost_stride", "unknown")
    reduct   = patterns.get("has_reduction", False)
    stencil  = patterns.get("has_stencil", False)
    tri      = patterns.get("has_triangular", False)
    matmul   = patterns.get("has_matmul_pattern", False)
    phi_c    = ir_patterns.get("phi_count", 0)
    gep_c    = ir_patterns.get("gep_count", 0)
    has_vec  = ir_patterns.get("has_vector_types", False)
    n_arr    = patterns.get("array_count", 0)

    # Rule 1: column-major access in deep loop → cache_inefficiency
    if depth >= 2 and stride == "column_major":
        inferred.append((
            "cache_inefficiency", 0.90,
            f"Column-major array access in depth-{depth} loop — "
            "innermost dimension strides across rows, causing cache misses. "
            "Consider loop interchange or tiling."
        ))

    # Rule 2: stencil patterns → memory_bound
    if stencil and depth >= 2:
        inferred.append((
            "memory_bound", 0.85,
            "Stencil access pattern detected (A[i±k][j]) — "
            "spatial locality limited by stencil footprint. "
            "Consider cache blocking / wavefront tiling."
        ))

    # Rule 3: deep matmul-style reduction, row-major → vectorization_gap
    if matmul and not stencil and stride == "row_major":
        inferred.append((
            "vectorization_gap", 0.82,
            f"Matrix-multiply pattern (depth={depth}, row-major access, reduction) — "
            "likely vectorizable but may need SLP/loop-vectorizer threshold tuning. "
            "Register blocking can improve SIMD utilization."
        ))

    # Rule 4: plain reduction without stencil → vectorization_gap
    if reduct and not stencil and not matmul:
        inferred.append((
            "vectorization_gap", 0.70,
            "Reduction loop without stencil — inner loop should vectorize. "
            "Check SLP threshold and loop-vectorize min-trip-count."
        ))

    # Rule 5: triangular bounds → branch_overhead
    if tri:
        inferred.append((
            "branch_overhead", 0.72,
            "Triangular loop bounds (k < i or similar) — "
            "loop-carried variable trip count prevents full unrolling. "
            "Loop peeling or conditional vectorization may help."
        ))

    # Rule 6: high GEP count relative to phi nodes → memory_bound (indirect)
    if phi_c > 0 and gep_c / max(phi_c, 1) > 4:
        inferred.append((
            "memory_bound", 0.60,
            f"High GEP/phi ratio ({gep_c}/{phi_c}={gep_c//max(phi_c,1)}×) — "
            "many memory accesses per loop iteration suggests memory bandwidth pressure."
        ))

    # Rule 7: many arrays, deep loops → cache_inefficiency
    if n_arr >= 3 and depth >= 3:
        inferred.append((
            "cache_inefficiency", 0.65,
            f"{n_arr} arrays accessed in depth-{depth} loops — "
            "working set may exceed L1/L2 cache. Loop tiling recommended."
        ))

    # Default: unknown
    if not inferred:
        inferred.append((
            "unknown", 0.40,
            "No clear static pattern detected. "
            "Try perf profiling for concrete bottleneck identification."
        ))

    # Sort by confidence descending
    inferred.sort(key=lambda x: -x[1])
    return inferred


# ── LLM-friendly formatting ───────────────────────────────────────────────────

def format_static_analysis_summary(patterns: dict,
                                    bottlenecks: List[Tuple[str, float, str]],
                                    ir_patterns: Optional[dict] = None) -> str:
    """
    Produce structured text for inclusion in the LLM prompt when perf data
    is not available. Clearly marks this as static inference (no runtime data).
    """
    lines = [
        "【静态分析（无运行时数据）】",
        "注：以下为源码/IR 结构推断，非硬件计数器数据。置信度低于实际 perf 测量。",
        "",
        "── 代码结构 ──",
    ]

    depth  = patterns.get("loop_depth", "?")
    stride = patterns.get("innermost_stride", "unknown")
    lvars  = ", ".join(patterns.get("loop_vars", []))
    lines.append(f"  循环嵌套深度: {depth}   循环变量: [{lvars}]")
    lines.append(f"  最内层访存步长: {stride}")

    flags = []
    if patterns.get("has_reduction"):   flags.append("归约(reduction)")
    if patterns.get("has_stencil"):     flags.append("模板(stencil)")
    if patterns.get("has_triangular"):  flags.append("三角边界(triangular)")
    if patterns.get("has_matmul_pattern"): flags.append("矩阵乘模式(matmul)")
    lines.append(f"  检测到的模式: {', '.join(flags) if flags else '无明显模式'}")
    lines.append(f"  访问的数组数量: {patterns.get('array_count', '?')}")

    if ir_patterns:
        lines.append("")
        lines.append("── IR 特征（O0，未优化）──")
        lines.append(f"  phi 节点(循环代理): {ir_patterns.get('phi_count', '?')}")
        lines.append(f"  GEP指令(内存访问): {ir_patterns.get('gep_count', '?')}")
        lines.append(f"  Load/Store: {ir_patterns.get('load_count','?')}/{ir_patterns.get('store_count','?')}")
        lines.append(f"  基础向量类型: {'已检测到' if ir_patterns.get('has_vector_types') else '无'}")

    lines.append("")
    lines.append("── 推断瓶颈（按置信度降序）──")
    for bt, conf, rationale in bottlenecks:
        lines.append(f"  [{conf:.0%}] {bt}: {rationale}")

    lines.append("")
    lines.append("── 建议优化方向 ──")
    if bottlenecks:
        primary = bottlenecks[0][0]
        if primary == "cache_inefficiency":
            lines.append("  1. 考虑循环交换(loop interchange)改善空间局部性")
            lines.append("  2. 考虑缓存分块(cache blocking/tiling)减少 cache miss")
            lines.append("  3. 检查 LoopInterchangePass / LICMPass 的 missed remarks")
        elif primary == "vectorization_gap":
            lines.append("  1. 降低 SLPVectorizerPass 的 -slp-threshold（允许更多向量化）")
            lines.append("  2. 降低 LoopVectorizePass 的 -vectorizer-min-trip-count")
            lines.append("  3. 考虑寄存器 blocking 提高 SIMD 利用率")
        elif primary == "memory_bound":
            lines.append("  1. 考虑 loop tiling 减少工作集大小")
            lines.append("  2. 检查 GVNPass 是否有 missed hoisting 机会")
            lines.append("  3. 考虑 prefetch 或数据布局变换（SOA vs AOS）")
        elif primary == "branch_overhead":
            lines.append("  1. 尝试增大 -unroll-threshold 展开三角循环")
            lines.append("  2. 考虑循环分裂(loop splitting)分离三角部分")

    return "\n".join(lines)
