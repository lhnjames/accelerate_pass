#!/usr/bin/env python3
"""
LLM source-level kernel optimizer with numerical equivalence verification.

Flow:
  1. Extract the kernel function from the PolyBench .c source
  2. LLM rewrites the kernel (loop reordering, tiling, register blocking, etc.)
  3. Compile BOTH versions with -DPOLYBENCH_DUMP_ARRAYS on SMALL_DATASET
  4. Compare numeric outputs element-wise (epsilon = 1e-4 relative + absolute)
  5. If correct, compile with -DPOLYBENCH_TIME on LARGE_DATASET and measure speedup
  6. On compile failure or precision failure, retry with error feedback to LLM
"""
import os, sys, re, argparse, subprocess, tempfile
from pathlib import Path

sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from src.config import ConfigLoader
from src.llm_client import LLMClient
from src.polybench_paths import find_polybench_utilities
from src.build_utils import run_timing, compile_c
from src.remarks import extract_rich_remarks_yaml, format_rich_remarks_for_source_prompt
from src.diagnostics import clean_clang_diagnostics


# ── helpers ───────────────────────────────────────────────────────────────────


def get_cpu_cache_info() -> str:
    """Return CPU model + cache sizes for LLM context."""
    lines = []
    try:
        with open("/proc/cpuinfo") as f:
            text = f.read()
        m = re.search(r"model name\s*:\s*(.+)", text)
        if m:
            lines.append(f"CPU: {m.group(1).strip()}")
        fm = re.search(r"^flags\s*:\s*(.+)", text, re.MULTILINE)
        if fm:
            flags = set(fm.group(1).split())
            if "avx512f" in flags:
                lines.append("SIMD: AVX-512 (8 doubles/vector)")
            elif "avx2" in flags:
                lines.append("SIMD: AVX2 (4 doubles/vector)")
    except Exception:
        pass
    try:
        r = subprocess.run(["lscpu"], capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            for ln in r.stdout.splitlines():
                if any(k in ln for k in ("L1d", "L2 ", "L3 ")):
                    lines.append(ln.strip())
    except Exception:
        pass
    return "\n".join(lines) if lines else "CPU info unavailable"


def extract_vectorization_remarks(clang: str, src: str,
                                   utils: Path, source_dir: Path) -> list:
    """
    Compile with -Rpass=loop-vectorize|slp-vectorizer|loop-unroll and return
    a list of strings like:
      "line 93: SLP vectorized possible but cost 0 >= 0 [slp-vectorizer]"
      "line 90: vectorized loop width=4 [loop-vectorize]"
    """
    polybench_c = utils / "polybench.c"
    cmd = [
        clang, "-O3", "-std=c99",
        f"-I{utils}", f"-I{source_dir}",
        "-DLARGE_DATASET", "-DPOLYBENCH_TIME",
        "-Rpass=loop-vectorize|slp-vectorizer|loop-unroll",
        "-Rpass-missed=loop-vectorize|slp-vectorizer|loop-unroll",
        "-Rpass-analysis=loop-vectorize|slp-vectorizer|loop-unroll",
        src, str(polybench_c),
        "-o", "/dev/null", "-lm",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    pattern = re.compile(
        r"([^\s:]+\.c):(\d+):\d+: remark: (.+?)\s+\[-R(pass(?:-missed|-analysis)?)=([\w.-]+)\]"
    )
    results = []
    seen = set()
    src_basename = os.path.basename(src)
    for line in r.stderr.splitlines():
        m = pattern.search(line)
        if not m:
            continue
        fpath, lineno, msg, rtype, rpass = m.groups()
        if os.path.basename(fpath) != src_basename:
            continue
        rtype_label = "MISSED" if "missed" in rtype else ("analysis" if "analysis" in rtype else "OK")
        key = (lineno, rtype_label, msg[:60])
        if key in seen:
            continue
        seen.add(key)
        results.append(f"  line {lineno} [{rtype_label}] {rpass}: {msg}")
    return results


def extract_kernel_function(content: str, kernel_name: str):
    """Return (kernel_source, start_idx, end_idx) by brace-counting.

    Anchored to the start of a line (mod leading whitespace) so a name that
    happens to also appear earlier in a comment (e.g. a license header
    reading "Copyright (c) ..." can regex-match a bare call/signature
    pattern like "Copyright(") can't be matched first -- see
    scripts/gen_tsvc_kernels.py's extract_function() for the same fix and
    a worked example (a commented-out stale signature spliced onto the
    real one).
    """
    pattern = r"^[ \t]*(?:static\s+)?[A-Za-z_][A-Za-z_0-9]*\s*\*?\s*" + re.escape(kernel_name) + r"\s*\("
    m = re.search(pattern, content, re.MULTILINE)
    if not m:
        return None, None, None

    # Find the matching close-paren for this signature's own "(" (paren-
    # counting, not just the next ")") -- needed to tell a genuine function
    # *definition* from a macro invocation/call statement that merely
    # happens to be followed, much later in the file, by some unrelated
    # "{". E.g. a PolyBench kernel's `POLYBENCH_2D(D,NI,NL,ni,nl);` array
    # declaration matches this same NAME(...)-shaped pattern; naively
    # searching for the next "{" after it can walk straight into an
    # unrelated function's body and silently return garbage labeled with
    # the macro's name (this is exactly how src/hotspot.py's call-graph
    # walk, which searches for arbitrary identifiers found via a bare
    # call-site regex, once mis-selected "POLYBENCH_2D" as 2mm's hot
    # function). A real function definition never has ';' immediately
    # after its parameter list's closing paren -- K&R-style parameter
    # declarations, if present, come first; a call/macro-invocation
    # statement does.
    paren_depth, paren_end = 1, -1
    for i in range(m.end(), len(content)):
        if content[i] == "(":
            paren_depth += 1
        elif content[i] == ")":
            paren_depth -= 1
            if paren_depth == 0:
                paren_end = i + 1
                break
    if paren_end == -1:
        return None, None, None
    # ';' -> a call/macro-invocation statement. ',' -> this "(...)" is one
    # item among several in a LARGER enclosing parameter list. ')' -> it's
    # the LAST item in that enclosing list instead (so what immediately
    # follows is the *enclosing* list's own closing paren, not this one's).
    # All three mean these parens were nested inside something else's
    # list, not a standalone function definition -- e.g. a PolyBench
    # kernel's own signature declares array parameters as
    # `DATA_TYPE POLYBENCH_2D(A,NI,NK,ni,nk), ..., DATA_TYPE
    # POLYBENCH_2D(D,NI,NL,ni,nl))`, where POLYBENCH_2D is a macro, not a
    # function, and every occurrence -- including the last, un-commaed one
    # -- matches this same NAME(...)-shaped, line-anchored pattern
    # (`DATA_TYPE` reads like a return type to the regex). A real
    # function's own parameter list is never immediately followed by any
    # of ';', ',', ')'.
    if content[paren_end:].lstrip()[:1] in (";", ",", ")"):
        return None, None, None

    brace_start = content.find("{", paren_end)
    if brace_start == -1:
        return None, None, None
    brace, end = 0, -1
    for i in range(brace_start, len(content)):
        ch = content[i]
        if ch == "{": brace += 1
        elif ch == "}":
            brace -= 1
            if brace == 0: end = i + 1; break
    if end == -1:
        return None, None, None
    return content[m.start():end], m.start(), end


def extract_header_macros(source_file: str) -> list:
    """Return sorted list of macro names defined in source and its local headers."""
    macro_names = set()
    source_dir = Path(source_file).parent

    def _parse(text: str):
        for m in re.finditer(r"^\s*#\s*define\s+(\w+)", text, re.MULTILINE):
            macro_names.add(m.group(1))

    with open(source_file) as f:
        src = f.read()
    _parse(src)
    for hdr in re.findall(r'#include\s+"([^"]+)"', src):
        p = source_dir / hdr
        if p.exists():
            _parse(p.read_text())
    return sorted(macro_names)


def extract_numbers_from_dump(text: str) -> "list | str":
    """
    Parse numeric values from POLYBENCH_DUMP_ARRAYS output.
    Returns a list of floats, or an error string if parsing fails.

    Markers come from polybench.h:
      POLYBENCH_DUMP_BEGIN(s) -> "begin dump: %s"        (no trailing newline --
                                  a single-value dump, e.g. a checksum, can
                                  trail the marker on the very same line)
      POLYBENCH_DUMP_END(s)   -> "\\nend   dump: %s\\n"   (note: three spaces,
                                  not one -- must match with variable whitespace)
    """
    import math
    float_re = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
    begin_re = re.compile(r"begin\s+dump:\s*\S*")
    end_re   = re.compile(r"end\s+dump:")
    lines = text.splitlines()
    has_markers = any(begin_re.search(l) or end_re.search(l) for l in lines)

    in_dump, values = False, []
    for line in lines:
        if end_re.search(line):
            in_dump = False; continue
        if begin_re.search(line):
            in_dump = True
            # Do NOT skip this line: a single-value dump (e.g. a checksum) has
            # no separator between the marker's name and the value, so the
            # value can trail directly on the same line. float_re below only
            # matches numeric tokens, so the leading marker text is naturally
            # skipped without needing to strip it explicitly.
        if in_dump or not has_markers:
            for tok in float_re.findall(line):
                try:
                    v = float(tok)
                    if math.isnan(v):
                        return "NaN in output"
                    if math.isinf(v):
                        return "Inf in output"
                    values.append(v)
                except ValueError:
                    pass
    return values


def compare_outputs(v1: list, v2: list, epsilon: float = 1e-4) -> tuple:
    """
    Compare two numeric output lists element-wise.
    Returns (ok, message).  Message includes statistics on failure.
    """
    import math

    # Check for error strings from extract_numbers_from_dump
    if isinstance(v1, str):
        return False, f"Reference output error: {v1}"
    if isinstance(v2, str):
        return False, f"Optimized output error: {v2}"

    if len(v1) == 0:
        return False, "Reference output is empty (binary may have crashed or produced no dump)"
    if len(v2) == 0:
        return False, "Optimized output is empty (binary may have crashed or produced no dump)"
    if len(v1) != len(v2):
        return False, (f"Size mismatch: reference={len(v1)}, optimized={len(v2)}. "
                       f"LLM likely changed the output array structure.")

    # Check for all-zeros (suspicious — suggests a bug that zeroes out the result)
    if all(v == 0.0 for v in v2) and not all(v == 0.0 for v in v1):
        return False, "Optimized output is all zeros but reference is not (likely a logic error)"

    max_diff, max_rel, max_idx = 0.0, 0.0, 0
    errors = []
    for idx, (a, b) in enumerate(zip(v1, v2)):
        diff  = abs(a - b)
        scale = max(abs(a), abs(b), 1e-9)
        rel   = diff / scale
        if diff > max_diff:
            max_diff = diff; max_idx = idx
        if rel > max_rel:
            max_rel = rel
        if diff > epsilon and rel > epsilon:
            errors.append((idx, a, b, diff, rel))

    if not errors:
        return True, "Outputs are numerically equivalent"

    # Report worst-case and statistics
    sample = errors[0]
    msg = (
        f"Mismatch: {len(errors)}/{len(v1)} elements differ > epsilon={epsilon}. "
        f"First error at index {sample[0]}: ref={sample[1]:.6g}, opt={sample[2]:.6g}, "
        f"abs={sample[3]:.2e}, rel={sample[4]:.2e}. "
        f"Max abs_diff={max_diff:.2e} at index {max_idx}. "
        f"Max rel_diff={max_rel:.2e}."
    )
    return False, msg


def _strip_fences(text: str) -> str:
    text = text.strip()
    for fence in ("```c", "```"):
        if text.startswith(fence): text = text[len(fence):]
    if text.endswith("```"): text = text[:-3]
    text = text.strip()
    # Strip any explanation text that appears before the function definition.
    # LLMs sometimes output prose like "Here is the optimized kernel:" before the code.
    m = re.search(r'(?:static\s+)?void\s+kernel_', text)
    if m and m.start() > 0:
        text = text[m.start():]
    return text.strip()


_ANALYSIS_SYSTEM = (
    "You are a compiler and CPU microarchitecture expert. "
    "Read C kernel code carefully and produce a precise, code-specific optimization plan."
)

_ANALYSIS_PROMPT = """\
Read the following C kernel and compiler remarks carefully. Your job is to find every
performance problem in THIS specific code and tell the rewrite engineer exactly what to do.

### Kernel source
```c
{kernel}
```

### Compiler remarks (-O3): what was optimized and what was missed
{remarks}

---

## What to look for (investigate ALL of these — only report what you actually find)

**1. Memory access stride in every array reference**
   For each array access in each loop nest, determine the stride when the innermost
   loop index increments. Stride-1 = good (sequential). Stride > 1 = cache miss.
   Ask: which loop index controls the LAST (fastest-varying) dimension of each array?
   If the innermost loop variable is NOT the last array index, there is a stride problem.

**2. Loop order vs. data layout**
   Arrays in C are row-major: A[i][j] stores rows contiguously. The innermost loop
   should always vary the LAST index. Check every loop nest: is the loop order
   consistent with row-major access for ALL arrays accessed in that loop?

**3. Inner loop trip count**
   Is the innermost loop's trip count a compile-time constant, a fixed runtime value,
   or does it depend on the outer loop variable (e.g., `j <= i`)? Variable trip count
   in the innermost loop prevents auto-vectorization.

**4. Data reuse across iterations**
   For each array loaded in the inner loop, ask: is the same element (or cache line)
   reused in the next iteration? If a value is loaded once but used many times, it
   should be hoisted. If a full row is reused across many inner iterations, tiling helps.

**5. Reduction structure**
   Find all accumulation patterns (acc += ...). Is the accumulator a scalar or array?
   Is it reset correctly between iterations? A scalar accumulator over the innermost
   loop is the canonical vectorizable form. Any other shape needs to be restructured.

**6. Computation that could be hoisted or pre-computed**
   Look for sub-expressions inside the inner loop that do not depend on the inner
   loop variable. These are loop-invariant and should be computed once outside.

**7. Loop fusion / fission opportunities**
   Are there consecutive loops over the same iteration range where the second loop
   reads data written by the first? Fusing them keeps the data in registers/L1.
   Conversely, if one loop does two unrelated things, splitting may help vectorization.

**8. Structural bottleneck**
   Classify the kernel: is it compute-bound (FLOP/byte ratio > machine balance)?
   Or memory-bandwidth bound? This determines whether the priority is vectorization
   or cache tiling.

---

## Output (be specific — name array variables and loop indices from the actual code)

BOTTLENECK: <single sentence identifying the main performance limiter>

FINDINGS:
- Memory: <describe each stride problem found, naming the array and loop index>
- Loop order: <describe any mismatch between loop nest and data layout>
- Trip count: <note any variable-length inner loops and which outer variable controls them>
- Reuse: <what data could be kept in registers or L1 with restructuring>
- Reduction: <describe the accumulation pattern and whether it is vectorizable>
- Hoist: <any loop-invariant expressions found inside inner loops>
- Other: <any other issue found>

OPTIMIZATION PLAN (ordered by expected impact, max 5 steps):
1. <concrete action using actual variable names from the code> — <why this helps>
2. ...

CONSTRAINTS (things that look helpful but will break correctness):
- <specific anti-pattern to avoid, with reason>
"""


def analyze_kernel_patterns(llm, kernel_src: str, vect_remarks: list,
                            max_tokens: int = 12800) -> str:
    """
    Call a dedicated analysis LLM to identify optimization patterns and produce
    a structured plan. Returns the plan text to be injected into the rewrite prompt.
    Retries up to 4 times with exponential backoff (handles API rate limits when
    multiple benchmark processes run concurrently).
    """
    import time, random
    remarks_text = "\n".join(vect_remarks[:40]) if vect_remarks else "(none)"
    prompt = _ANALYSIS_PROMPT.format(kernel=kernel_src, remarks=remarks_text)
    max_attempts = 14
    for attempt in range(max_attempts):
        if attempt > 0:
            delay = min(2 ** min(attempt, 5), 32) + random.uniform(0, 2)
            time.sleep(delay)
        resp = llm.call(
            [{"role": "system", "content": _ANALYSIS_SYSTEM},
             {"role": "user",   "content": prompt}],
            max_tokens=max_tokens,
            timeout=240,
            temperature=0,  # deterministic analysis — same kernel → same plan every time
        )
        if resp and resp.strip():
            return resp.strip()
        print(f"  (analysis attempt {attempt+1}/{max_attempts} returned nothing, retrying...)")
    return ""


def _detect_triangular_loop(kernel_src: str) -> bool:
    """Return True if kernel has a triangular loop bound like j<=i or j<i."""
    return bool(re.search(r'for\s*\([^)]*;\s*\w+\s*<=?\s*\w+\s*;', kernel_src)
                and re.search(r'j\s*<=?\s*i|i\s*>=?\s*j', kernel_src))


def _detect_inplace_stencil(kernel_src: str) -> bool:
    """Return True if kernel writes array[i][j] and also reads array[i±1][j±1] in-place.

    Catches Gauss-Seidel, ADI column sweeps, and similar stencils where the
    update order is part of the algorithm's semantics.
    """
    _LOOP_VARS = {'i', 'j', 'k', 't', 'n', 'm', 'p', 'q', 'r', 's'}
    candidate_arrays = set(re.findall(r'\b([A-Za-z]\w*)\s*\[', kernel_src)) - _LOOP_VARS
    for arr in candidate_arrays:
        # Does the same array appear with a ±1 offset on either dimension?
        if re.search(rf'\b{re.escape(arr)}\s*\[[^\]]*[+\-]\s*1[^\]]*\]', kernel_src):
            return True
    return False


def _detect_inplace_factorization(kernel_src: str) -> bool:
    """Return True if kernel looks like an in-place LU/Cholesky factorization.

    Detects the pattern  A[i][j] -= A[i][k] * A[k][j]  (or similar),
    where the same matrix is both read and written in the same loop body.
    """
    # LU: A[i][j] -= A[i][k] * A[k][j]
    if re.search(r'(\w+)\s*\[[^\]]+\]\s*\[[^\]]+\]\s*-=\s*\1\s*\[', kernel_src):
        return True
    # Cholesky: A[k][k] = sqrt(...) followed by A[i][k] /= A[k][k]
    if (re.search(r'\bsqrt\s*\(', kernel_src)
            and re.search(r'(\w+)\s*\[[^\]]+\]\s*\[[^\]]+\]\s*/=\s*\1\s*\[', kernel_src)):
        return True
    return False


def _detect_strided_inner_loop(kernel_src: str):
    """Heuristic loop-interchange opportunity detector.

    Finds the innermost loop variable (the last `for (var = ...)` header in
    textual order -- true for the simple nested-loop shape PolyBench/TSVC/
    cBench kernels use, where loops are written in nesting order with no
    sibling loops interleaved) and checks whether 2D array accesses in the
    kernel use that variable as the FIRST subscript (`A[iv][j]`, stride-N
    across a row-major array) rather than the LAST one (`A[j][iv]`,
    stride-1/contiguous). The former is the textbook symptom loop
    interchange fixes: swapping loop order so the innermost variable walks
    the LAST subscript turns strided access into sequential access,
    unlocking both cache-line reuse and auto-vectorization -- see symm's
    9.8x-from-one-rewrite jump when the agent found this on its own vs.
    the ~4.5x ceiling of tiling-only attempts that never revisited it.
    Only flags arrays where the strided pattern isn't itself balanced out
    by a contiguous access to the same array elsewhere in the kernel
    (which would mean the access pattern is genuinely mixed, not a
    plain oversight). Returns (found, message).
    """
    loop_vars = re.findall(r'\bfor\s*\(\s*(?:int\s+)?(\w+)\s*=', kernel_src)
    if not loop_vars:
        return False, ""
    iv = loop_vars[-1]
    # arr -> companion var seen in the OTHER subscript slot when iv is the
    # strided (first-subscript) one -- this is the variable that should
    # become the new innermost loop, not iv itself (iv is already
    # innermost; the fix is to swap it out, not "move it in").
    strided: dict = {}
    contiguous_arrays = set()
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\[([^\[\]]+)\]\s*\[([^\[\]]+)\]', kernel_src):
        arr, idx1, idx2 = m.group(1), m.group(2).strip(), m.group(3).strip()
        iv_first = bool(re.fullmatch(rf'{re.escape(iv)}(\s*[+\-]\s*\w+)?', idx1))
        iv_last  = bool(re.fullmatch(rf'{re.escape(iv)}(\s*[+\-]\s*\w+)?', idx2))
        other = re.match(r'(\w+)', idx2).group(1) if idx2 else None
        if iv_first and not re.search(rf'\b{re.escape(iv)}\b', idx2) and other and other != iv:
            strided.setdefault(arr, other)
        elif iv_last and not re.search(rf'\b{re.escape(iv)}\b', idx1):
            contiguous_arrays.add(arr)
    flagged = {a: v for a, v in strided.items() if a not in contiguous_arrays}
    if not flagged:
        return False, ""
    example, companion = sorted(flagged.items())[0]
    arrs = ", ".join(sorted(flagged))
    return True, (
        f"检测到内层循环变量 `{iv}` 在数组 {arrs} 里被用作第一个下标（如 `{example}[{iv}][{companion}]`），"
        f"这是行主序（row-major）布局下的跨步访问（stride-N，不连续，阻碍向量化和 cache line 复用）。"
        f"这类模式通常可以通过**循环交换**（把 `{iv}` 和 `{companion}` 的嵌套顺序对调，让 `{companion}` "
        f"变成最内层循环，`{iv}` 挪到外层/中层）直接把这些数组的访问改成连续的 `{example}[...][{iv}]` 形式——"
        f"往往比单纯 tiling/分块收益大得多，值得优先尝试，而不是只在当前访问模式上做 cache blocking。")


_PRECISION_ANALYSIS_SYSTEM = (
    "You are a floating-point numerical analysis expert. "
    "You diagnose exactly why two C kernels produce different numeric outputs "
    "when compiled with clang -O3."
)

_PRECISION_ANALYSIS_PROMPT = """\
Two C kernels compiled with `clang -O3` should produce identical floating-point \
output, but the optimized version does not match the reference.
Your job: read BOTH kernels carefully, find every structural difference, \
and determine which difference causes the numeric divergence.

### Reference kernel (correct output)
```c
{original}
```

### Optimized kernel (wrong output)
```c
{optimized}
```

### Measured divergence
```
{error}
```

## Background: what causes FP divergence in compiled C

Floating-point arithmetic is NOT associative. The compiler produces results that depend on \
the exact order additions/multiplications happen. Any transformation that changes that order \
changes the result. Common sources:

- `#pragma clang loop interleave(enable)` on a reduction loop forces the compiler to use \
  multiple accumulators (e.g. acc0, acc1 interleaved), then sum them at the end. \
  This changes the addition order vs a single sequential accumulator.
- `#pragma clang loop vectorize(enable)` on a reduction `acc -= A[k]*B[k]` causes SIMD \
  lane-wise accumulation followed by a horizontal reduction — different order than scalar.
  These pragmas are safe on non-reduction loops (initialization, copy, scale).
- Tiling the reduction dimension (k in `for k: acc += A[i][k]*B[k][j]`) changes summation \
  order. Only tile output dimensions (i, j).
- In-place sequential algorithms (Cholesky, LU, Gauss-Seidel): each row i depends on ALL \
  previously written rows 0..i-1. Reordering the outer i loop or the j loop relative to i \
  reads not-yet-computed values → wrong results. The outer loop order is semantically fixed.
- Changing the function signature (replacing `DATA_TYPE POLYBENCH_2D(A,N,N,n,n)` with \
  `double A[n][n]`) alters how the PolyBench harness dumps the output array → size mismatch.
- Using `restrict` on pointers that alias each other may cause the compiler to reorder \
  memory accesses in ways that change intermediate values.

## Your task

1. List EVERY structural difference between the two kernels (loop bounds, pragmas, \
   accumulators, tiling, signatures, added qualifiers).
2. For each difference, state whether it changes floating-point computation order \
   and why.
3. Identify the ONE (or more) differences that explain the measured divergence.
4. State the minimal fix.

Answer in this format:

DIFFERENCES:
- <diff 1: what changed and where>
- <diff 2: ...>
...

ROOT CAUSE: <which difference causes FP divergence, and the exact mechanism>

FIX: <the minimal code change needed — what to remove, add, or revert — NOT a full rewrite>

SAFE TO KEEP: <list the optimizations in the failed version that are numerically correct and should be preserved>
"""


def analyze_precision_failure(llm, original_kernel: str, failed_code: str,
                               error_msg: str) -> str:
    """
    Call a dedicated LLM to diagnose why the optimized kernel produces wrong results.
    Returns a short diagnosis string (4-sentence structured format).
    """
    import time, random
    prompt = _PRECISION_ANALYSIS_PROMPT.format(
        original=original_kernel, optimized=failed_code, error=error_msg[:300])
    for attempt in range(6):
        if attempt > 0:
            time.sleep(min(2 ** attempt, 16) + random.uniform(0, 1))
        resp = llm.call(
            [{"role": "system", "content": _PRECISION_ANALYSIS_SYSTEM},
             {"role": "user",   "content": prompt}],
            max_tokens=2048,
            timeout=120,
            temperature=0,
        )
        if resp and resp.strip():
            return resp.strip()
    return ""


def _build_precision_fix_prompt(kernel_name: str, original_kernel: str,
                                failed_code: str, error_msg: str,
                                diagnosis: str = "",
                                precision_history: "list | None" = None) -> str:
    """Fix prompt: original + failed code + diagnosis + accumulated failure history."""
    history_block = ""
    if precision_history:
        items = []
        for h in precision_history:
            items.append(
                f"Attempt {h['attempt']} failed:\n"
                f"  Error: {h['error'][:120]}\n"
                f"  Diagnosis: {h['diagnosis'][:300] if h['diagnosis'] else '(not diagnosed)'}"
            )
        history_block = (
            "\n### Precision failure history — NEVER repeat these patterns\n"
            + "\n\n".join(items) + "\n"
        )

    diagnosis_block = ""
    if diagnosis:
        diagnosis_block = f"\n### Expert diagnosis of this failure\n{diagnosis}\n"

    return f"""Your optimized C kernel introduced a numerical precision error.
Fix the precision while keeping as much performance optimization as possible.
{history_block}{diagnosis_block}
### Original kernel (numerically correct reference)
```c
{original_kernel}
```

### Your optimized version (fast but numerically wrong)
```c
{failed_code}
```

### Precision failure
```
{error_msg}
```

## Rules (read the diagnosis above first — it tells you the specific cause)
1. **Reduction loop pragmas**: NEVER add `vectorize(enable)` or `interleave(enable)` to
   any loop that accumulates into a scalar (`acc -= A[k]*B[k]`). These force multi-accumulator
   SIMD reduction that changes floating-point summation order vs the scalar reference.
   Safe: add vectorize pragma ONLY to non-reduction loops (initialization, copy, scale loops).
2. **Never tile the k (reduction) dimension** — tile only output dimensions (i, j).
3. **Exact function signature** — same name `{kernel_name}`, same parameter names and types,
   same macro forms (e.g. `DATA_TYPE POLYBENCH_2D(A,N,N,n,n)` not `double A[n][n]`).
4. **Single accumulator per output element** — one `double acc` only, no acc0+acc1.
5. Keep all other valid optimizations (row pointers, i/j-tiling, cache-friendly ordering).

Output ONLY the fixed kernel function. No prose, no markdown fences.
Start immediately with the function definition.
"""


def _build_compile_fix_prompt(kernel_name: str, original_kernel: str,
                              failed_code: str, error_msg: str) -> str:
    """
    Fix prompt for a plain compile/link failure (syntax, type, undefined
    symbol, etc.) in a rewrite_source candidate -- distinct from
    _build_precision_fix_prompt, which is for candidates that compiled fine
    but produced numerically wrong output. Kept deliberately generic: the
    compiler error message is usually self-explanatory (unlike precision
    bugs, which need a diagnosis pass to find the root cause).
    """
    return f"""Your optimized C kernel failed to compile.
Fix ONLY the compile error, keeping as much of the intended optimization as possible.

### Original kernel (known to compile and run correctly)
```c
{original_kernel}
```

### Your optimized version (does not compile)
```c
{failed_code}
```

### Compiler/linker error
```
{error_msg[:800]}
```

## Rules
1. Fix the exact error above -- e.g. if it's "array type ... is not assignable", you
   assigned directly to an array-typed variable (or dereferenced array-typed pointer);
   arrays can only be copied element-by-element or manipulated via pointers to their
   element type, never assigned as a whole.
2. **Exact function signature** — same name `{kernel_name}`, same parameter names and
   types as the original.
3. Do not introduce calls to functions or symbols that aren't already used elsewhere
   in this file -- undefined-reference errors mean the symbol doesn't exist here.
4. Keep every other optimization from your previous attempt that isn't implicated by
   the error.

Output ONLY the fixed kernel function. No prose, no markdown fences.
Start immediately with the function definition.
"""


def _build_prompt(kernel_name: str, original_kernel: str,
                  macro_names: list, vect_remarks: list,
                  prev_error: "str | None", attempt: int,
                  cpu_cache_info: str = "",
                  param_context: str = "",
                  extra_hints: str = "",
                  rich_remarks: "dict | None" = None,
                  precision_history: "list | None" = None,
                  attempt_history: "list | None" = None) -> str:
    """Build the LLM prompt, injecting error feedback when retrying."""
    forbidden = ", ".join(f"`{m}`" for m in macro_names[:30]) if macro_names else "none"
    if rich_remarks:
        remarks_block = format_rich_remarks_for_source_prompt(rich_remarks, max_missed=8)
    else:
        remarks_block = ("\n".join(vect_remarks[:50]) if vect_remarks else "  (none)")

    error_section = ""
    if prev_error:
        error_section = f"""
### PREVIOUS ATTEMPT FAILED — Fix the following error before writing code
```
{prev_error[:800]}
```
"""

    precision_history_section = ""
    if precision_history:
        lines = ["### Precision failure history — NEVER repeat these patterns"]
        for h in precision_history:
            lines.append(
                f"  Attempt {h['attempt']}: {h['error'][:100]}"
            )
            if h.get("diagnosis"):
                for dl in h["diagnosis"].splitlines()[:4]:
                    lines.append(f"    {dl}")
        lines.append(
            "  → Do NOT add `vectorize(enable)` or `interleave(enable)` to reduction loops."
        )
        precision_history_section = "\n".join(lines) + "\n"

    attempt_history_section = ""
    if attempt_history:
        lines = ["### All previous source-round results (learn from what worked and what didn't)"]
        for h in attempt_history:
            sp = h.get("speedup", 1.0)
            status = h.get("status", "?")
            err = h.get("error", "")
            if status == "ok":
                lines.append(f"  Round {h['attempt']}: {sp:.3f}x — PASSED")
            else:
                lines.append(f"  Round {h['attempt']}: FAILED ({status}) — {err[:100]}")
        attempt_history_section = "\n".join(lines) + "\n"

    cache_section  = f"\n### Target hardware\n{cpu_cache_info}\n" if cpu_cache_info else ""
    param_section  = param_context if param_context else ""

    # Fallback hints only when no analysis was provided by the analysis LLM
    triangular_hint = ""
    if not extra_hints and _detect_triangular_loop(original_kernel):
        triangular_hint = (
            "\n### CRITICAL: triangular loop detected (j<=i)\n"
            "Put k as the INNERMOST loop so A[i][k] and A[j][k] are both stride-1.\n"
            "Tile i and j (NOT k). Never keep j as inner with k fixed — stride-M cache miss.\n"
        )

    inplace_hint = ""
    if _detect_inplace_stencil(original_kernel):
        inplace_hint = (
            "\n### CRITICAL: in-place stencil detected\n"
            "This kernel reads A[i±1][j±1] and writes A[i][j] in the SAME pass.\n"
            "The iteration order (e.g. t→i→j) is SEMANTICALLY MEANINGFUL — each cell\n"
            "uses already-updated neighbors from the current sweep (Gauss-Seidel style).\n"
            "DO NOT: snapshot A into a temporary array before updating (that converts it\n"
            "  to Jacobi iteration, a different algorithm with different results).\n"
            "DO NOT: reorder the i/j loops or tile the t dimension across i/j.\n"
            "Safe optimizations: prefetch, scalar replacement of A[i][j] within one row,\n"
            "  register-block along j (but keep i sequential and forward).\n"
        )
    elif _detect_inplace_factorization(original_kernel):
        inplace_hint = (
            "\n### CRITICAL: in-place matrix factorization detected\n"
            "This kernel updates matrix A in-place (LU/Cholesky style). Later iterations\n"
            "of the OUTER loop depend on values written by earlier iterations of the SAME\n"
            "outer loop. The k-loop order is SEMANTICALLY MANDATORY.\n"
            "DO NOT: add `#pragma clang loop vectorize(enable)` or `ivdep` to ANY loop\n"
            "  that reads and writes overlapping elements of the same matrix.\n"
            "DO NOT: parallelize or reorder the k (pivot) loop.\n"
            "Safe: reorder i/j inside a fixed k, cache A[k][*] row, row-pointer tricks.\n"
        )

    extra_section = f"\n{extra_hints.strip()}\n" if extra_hints.strip() else ""

    return f"""You are a performance engineering expert specializing in high-performance C.

Optimize the following PolyBench kernel function for speed when compiled with \
`clang -O3` on the target CPU (see hardware info below).
{cache_section}{param_section}{triangular_hint}{inplace_hint}{extra_section}{attempt_history_section}{precision_history_section}{error_section}
### Original kernel (attempt {attempt})
```c
{original_kernel}
```

### Optimization remarks from clang -O3 (with source code location)
These show EXACTLY what was optimized, what was MISSED, and the source lines involved:
{remarks_block}

### Optimization strategies (ranked by typical impact)
1. **Loop reordering**: Reorder i/j/k so the innermost loop has stride-1 memory access
   (C arrays are row-major — fastest when innermost index is the last array dimension).
2. **Cache tiling**: Tile with block size 32–64 to fit working set in L1 (32 KB).
   For matrix multiply: typical tile = 32×32 (fits 3×32×32×8 = 24 KB < L1).
3. **Register blocking**: Accumulate multiple output elements per inner loop
   to increase ILP — use separate `acc_ij` variables, ONE per output cell.
4. **Row pointers**: Replace `A[i][k]` with `double *Ai = A[i]; Ai[k]` to help
   the compiler eliminate repeated multiply-add address calculations.
5. **Local accumulator**: `double acc = C[i][j]; for(k) acc += ...; C[i][j] = acc;`

### STRICT REQUIREMENTS (any violation = immediate failure)
- **Exact function signature** — same name `{kernel_name}`, same parameter names and types.
- **Single accumulator per output element**:
  NEVER: `acc0 += A[k]; acc1 += B[k]; result = acc0 + acc1;` ← changes FP order → wrong
  RIGHT: `double acc = 0.0; for(k) acc += A[k]*B[k]; C[i][j] = alpha*acc + ...;`
- **NEVER tile the reduction (k) dimension**: In a dot-product loop like
  `for(k) sum += A[i][k]*B[k][j]`, the k-loop MUST iterate 0…nk in the SAME ORDER.
  Tiling k changes floating-point summation order → wrong results.
  You MAY tile i and j (output dimensions), but NOT k (reduction dimension).
- **FORBIDDEN variable names** (these are `#define` macros — using them causes errors):
  {forbidden}
  Use `ii`, `jj`, `kk`, `bi`, `bj`, `bk`, `tile`, `TILE_SZ`, `blk` instead.
- **Valid clang pragmas ONLY** — any other pragma causes a compile error:
  ALLOWED: `#pragma clang loop vectorize(enable)`, `#pragma clang loop interleave(enable)`,
           `#pragma clang loop unroll(enable)`, `#pragma clang loop unroll_count(N)`
  FORBIDDEN: `#pragma GCC ivdep`, `#pragma ivdep`, `#pragma omp`, any OpenMP directive,
             `#pragma clang loop ivdep` (does not exist in clang).
- Output ONLY the single kernel function — no `main`, no `init_array`, no helpers.
- Start IMMEDIATELY with the function definition `void kernel_...` or `static void kernel_...`.
  Do NOT write any explanation, comment, or prose before the opening line.
- End with the closing `}}`.
- Do NOT wrap in markdown code fences.
"""


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="LLM source-level kernel rewriter with correctness verification")
    parser.add_argument("--program",    required=True)
    parser.add_argument("--config",     default="configs")
    parser.add_argument("--runs",       type=int,   default=5)
    parser.add_argument("--pin-cpu",    type=int,   default=None)
    parser.add_argument("--epsilon",    type=float, default=1e-4)
    parser.add_argument("--iterations", type=int,   default=3,
                        help="Max LLM rewrite attempts (default: 3)")
    parser.add_argument("--max-tokens", type=int,   default=8000)
    parser.add_argument("--param-flags", type=str,  default="",
                        help="Best -mllvm flags from param round (e.g. '-slp-threshold=-1'). "
                             "These are added to ALL compilation steps so source+param compound.")
    parser.add_argument("--param-analysis", type=str, default="",
                        help="One-line analysis from param round, added to LLM prompt context.")
    args = parser.parse_args()

    loader = ConfigLoader(config_dir=os.path.abspath(args.config))
    config = loader.load_all()
    pin_cpu = args.pin_cpu if args.pin_cpu is not None else config.runtime.pin_cpu
    clang   = config.compiler.clang_path

    # Parse param flags string → list of ["-mllvm", "flag=val", ...]
    param_extra_flags = []
    if args.param_flags.strip():
        for token in args.param_flags.strip().split():
            if token.startswith("-mllvm"):
                param_extra_flags.append(token)
            else:
                param_extra_flags += ["-mllvm", token]

    src = args.program
    if not os.path.exists(src): sys.exit(f"Not found: {src}")
    name        = Path(src).stem
    kernel_name = f"kernel_{name.replace('-', '_')}"
    utils       = find_polybench_utilities(src)
    if not utils: sys.exit("PolyBench utilities not found")
    polybench_c = utils / "polybench.c"
    source_dir  = Path(src).resolve().parent
    include_dirs = [utils, source_dir]

    with open(src) as f:
        source_code = f.read()
    original_kernel, start_idx, end_idx = extract_kernel_function(source_code, kernel_name)
    if not original_kernel:
        sys.exit(f"Could not extract '{kernel_name}' from {src}")
    print(f"Extracted kernel '{kernel_name}' ({end_idx - start_idx} chars).")

    macro_names = extract_header_macros(src)
    print(f"Forbidden macros: {macro_names}")

    # Rich YAML remarks (source snippets + VF/IC) — fall back to text remarks if unavailable
    print("Collecting optimization remarks (clang -O3 YAML)...")
    rich_remarks = extract_rich_remarks_yaml(clang, src, utils, source_dir, kernel_name)
    if rich_remarks:
        for pname, entries in sorted(rich_remarks.items(),
                                     key=lambda kv: -len([e for e in kv[1] if e["type"] == "missed"])):
            missed_n = len([e for e in entries if e["type"] == "missed"])
            passed_n = len([e for e in entries if e["type"] == "passed"])
            if missed_n > 0 or passed_n > 0:
                print(f"  {pname}: {missed_n} missed, {passed_n} applied")
                for e in entries[:2]:
                    if e["type"] == "missed" and e.get("source_snippet"):
                        print(f"    {e['file']}:{e['line']}: {e['msg'][:60]}")
        vect_remarks = []  # rich_remarks replaces vect_remarks
    else:
        print("  (YAML unavailable — falling back to text remarks)")
        vect_remarks = extract_vectorization_remarks(clang, src, utils, source_dir)
        print(f"  {len(vect_remarks)} text remarks")
        for r in vect_remarks[:5]:
            print(f"    {r}")

    # CPU/cache info for LLM
    cpu_cache_info = get_cpu_cache_info()

    llm = LLMClient(config.llm)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        # Reference binaries
        # Note: reference uses plain -O3 (no param flags) — correctness baseline
        # Timing reference also uses plain -O3 so speedup is "source vs O3-only"
        print("Compiling reference binaries...")
        if param_extra_flags:
            print(f"  Param flags active (added to all builds): {' '.join(param_extra_flags)}")
        orig_dump_bin = tmpdir / "orig_dump"
        orig_time_bin = tmpdir / "orig_time"
        ok, err = compile_c(clang, [src, str(polybench_c)], include_dirs,
                            ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"],
                            orig_dump_bin)
        if not ok: sys.exit(f"Reference dump compile failed:\n{err}")
        ok, err = compile_c(clang, [src, str(polybench_c)], include_dirs,
                            ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"],
                            orig_time_bin, extra_flags=param_extra_flags)
        if not ok: sys.exit(f"Reference time compile failed:\n{err}")

        try:
            res_ref = subprocess.run([str(orig_dump_bin)],
                                     capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            sys.exit("Reference dump binary timed out (60s)")
        ref_out  = (res_ref.stdout or "") + (res_ref.stderr or "")
        ref_nums = extract_numbers_from_dump(ref_out)
        if isinstance(ref_nums, str):
            sys.exit(f"Reference dump error: {ref_nums}")
        if len(ref_nums) < 10:
            sys.exit(f"Reference dump too few values ({len(ref_nums)}) — "
                     "binary may have crashed or POLYBENCH_DUMP_ARRAYS not defined")
        print(f"Reference dump: {len(ref_nums)} values.")

        orig_time = run_timing(str(orig_time_bin), runs=args.runs, pin_cpu=pin_cpu)
        if orig_time <= 0: sys.exit("Failed to measure reference time.")
        print(f"Reference -O3:  {orig_time:.2f} ms")

        best_time, best_source = orig_time, None
        prev_error = None
        precision_failures: list = []  # accumulated across all attempts

        for attempt in range(1, args.iterations + 1):
            print(f"\n[Attempt {attempt}/{args.iterations}] Querying LLM...")

            # Include param analysis context if available
            param_ctx = ""
            if args.param_analysis.strip():
                param_ctx = f"\n### Compiler param analysis (from previous tuning round)\n{args.param_analysis.strip()}\n"
            if param_extra_flags:
                param_ctx += (f"\nNote: all builds use extra flags {' '.join(param_extra_flags)} "
                              f"— the compiler already applies these, so your source changes "
                              f"should provide ADDITIONAL improvement beyond what those flags give.\n")

            prompt = _build_prompt(kernel_name, original_kernel, macro_names,
                                   vect_remarks, prev_error, attempt,
                                   cpu_cache_info=cpu_cache_info,
                                   param_context=param_ctx,
                                   rich_remarks=rich_remarks if rich_remarks else None,
                                   precision_history=precision_failures or None)
            response = llm.call([
                {"role": "system",
                 "content": "You are a performance code rewriter. Output C code only."},
                {"role": "user", "content": prompt},
            ], max_tokens=args.max_tokens)

            if not response:
                print("  [WARN] LLM returned no response.")
                continue

            opt_code = _strip_fences(response)
            print("Optimized code from LLM:")
            print("-" * 60)
            print(opt_code[:2000])
            if len(opt_code) > 2000:
                print("...(truncated)")
            print("-" * 60)

            opt_source   = source_code[:start_idx] + opt_code + source_code[end_idx:]
            opt_src_path = tmpdir / f"{name}_opt_{attempt}.c"
            with open(opt_src_path, "w") as f:
                f.write(opt_source)

            # Correctness check — dump on SMALL_DATASET (without param flags to avoid
            # noise from SIMD rounding; numerical equivalence is vs plain -O3 reference)
            print("Verifying correctness on SMALL_DATASET...")
            opt_dump_bin = tmpdir / f"opt_dump_{attempt}"
            ok, compile_err = compile_c(
                clang, [str(opt_src_path), str(polybench_c)], include_dirs,
                ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"], opt_dump_bin)
            if not ok:
                prev_error = f"COMPILE ERROR:\n{clean_clang_diagnostics(compile_err)}"
                print(f"  Compile FAILED:\n{clean_clang_diagnostics(compile_err, max_diagnostics=3)}")
                continue

            try:
                res_opt = subprocess.run([str(opt_dump_bin)],
                                         capture_output=True, text=True, timeout=60,
                                         errors="replace")
            except subprocess.TimeoutExpired:
                prev_error = "RUNTIME ERROR: binary timed out during correctness dump (60s)"
                print("  Run TIMEOUT during dump.")
                continue

            opt_out  = (res_opt.stdout or "") + (res_opt.stderr or "")
            opt_nums = extract_numbers_from_dump(opt_out)

            if isinstance(opt_nums, str):
                prev_error = f"RUNTIME ERROR: {opt_nums}"
                print(f"  Output error: {opt_nums}")
                continue

            print(f"  Reference: {len(ref_nums)} vals  |  Optimized: {len(opt_nums)} vals")

            ok_eq, msg = compare_outputs(ref_nums, opt_nums, epsilon=args.epsilon)
            if not ok_eq:
                print(f"  Verification FAILED: {msg}")
                # ── step 1: diagnosis LLM ─────────────────────────────────
                print("  [Precision Analysis] Diagnosing root cause...")
                diagnosis = analyze_precision_failure(llm, original_kernel, opt_code, msg)
                if diagnosis:
                    print(f"  [Precision Analysis]\n    "
                          + "\n    ".join(diagnosis.splitlines()[:6]))
                # record for history (used by future main-prompt attempts)
                precision_failures.append({
                    "attempt": attempt,
                    "code":    opt_code,
                    "error":   msg,
                    "diagnosis": diagnosis,
                })
                # ── step 2: fix LLM ───────────────────────────────────────
                print("  [Precision Fix] Querying LLM to fix error...")
                fix_prompt = _build_precision_fix_prompt(
                    kernel_name, original_kernel, opt_code, msg,
                    diagnosis=diagnosis,
                    precision_history=precision_failures[:-1] or None)
                fix_resp = llm.call([
                    {"role": "system",
                     "content": "You are a C performance expert. Fix the precision error. "
                                "Output C code only."},
                    {"role": "user", "content": fix_prompt},
                ], max_tokens=args.max_tokens)
                precision_fixed = False
                if fix_resp:
                    fixed_code   = _strip_fences(fix_resp)
                    fixed_source = source_code[:start_idx] + fixed_code + source_code[end_idx:]
                    fixed_src_path = tmpdir / f"{name}_fix_{attempt}.c"
                    with open(fixed_src_path, "w") as f:
                        f.write(fixed_source)
                    fixed_dump_bin = tmpdir / f"fix_dump_{attempt}"
                    ok2, err2 = compile_c(
                        clang, [str(fixed_src_path), str(polybench_c)], include_dirs,
                        ["-DPOLYBENCH_DUMP_ARRAYS", "-DSMALL_DATASET"], fixed_dump_bin)
                    if ok2:
                        try:
                            res_fix = subprocess.run(
                                [str(fixed_dump_bin)], capture_output=True,
                                text=True, timeout=60, errors="replace")
                            fix_out  = (res_fix.stdout or "") + (res_fix.stderr or "")
                            fix_nums = extract_numbers_from_dump(fix_out)
                            ok_fix, fix_msg = compare_outputs(
                                ref_nums, fix_nums, epsilon=args.epsilon)
                            if ok_fix:
                                print(f"  [Precision Fix] PASSED ({fix_msg})")
                                opt_code     = fixed_code
                                opt_src_path = fixed_src_path
                                opt_source   = fixed_source
                                precision_fixed = True
                            else:
                                print(f"  [Precision Fix] Still FAILED: {fix_msg}")
                                # record fix attempt too so history grows
                                precision_failures.append({
                                    "attempt": f"{attempt}fix",
                                    "code":    fixed_code,
                                    "error":   fix_msg,
                                    "diagnosis": "",
                                })
                        except subprocess.TimeoutExpired:
                            print("  [Precision Fix] Timeout during dump.")
                    else:
                        print(f"  [Precision Fix] Compile FAILED:\n{clean_clang_diagnostics(err2, max_diagnostics=3)}")

                if not precision_fixed:
                    prev_error = (
                        f"PRECISION ERROR: {msg}\n"
                        f"ROOT CAUSE (from analysis): {diagnosis[:300] if diagnosis else 'unknown'}\n"
                        f"Do NOT repeat this pattern in the next attempt."
                    )
                    continue
                prev_error = None
            else:
                print(f"  Verification PASSED ({msg})")
                prev_error = None

            # Performance — compile WITH param flags so speedup is "source+param vs param-only"
            print("Measuring performance on LARGE_DATASET...")
            opt_time_bin = tmpdir / f"opt_time_{attempt}"
            ok, err = compile_c(
                clang, [str(opt_src_path), str(polybench_c)], include_dirs,
                ["-DPOLYBENCH_TIME", "-DLARGE_DATASET"], opt_time_bin,
                extra_flags=param_extra_flags)
            if not ok:
                print(f"  Time-compile FAILED:\n{clean_clang_diagnostics(err, max_diagnostics=3)}")
                continue

            opt_time = run_timing(str(opt_time_bin), runs=args.runs, pin_cpu=pin_cpu)
            if opt_time <= 0:
                print("  Timing run FAILED.")
                continue

            sp = orig_time / opt_time
            print(f"  Attempt {attempt}: {opt_time:.2f} ms  ({sp:.3f}x / {(sp-1)*100:+.1f}%)")
            if opt_time < best_time:
                best_time = opt_time; best_source = opt_source
                print("  --> New best!")

        # Report
        print("\n" + "=" * 60)
        print(f"Program:      {name}")
        print(f"Original -O3: {orig_time:.2f} ms")
        if best_source:
            sp = orig_time / best_time
            print(f"Optimized:    {best_time:.2f} ms")
            print(f"Speedup:      {sp:.3f}x ({(sp-1)*100:+.1f}%)")
            out_dir = Path("outputs"); out_dir.mkdir(exist_ok=True)
            out_file = out_dir / f"{name}_optimized.c"
            with open(out_file, "w") as f: f.write(best_source)
            print(f"Saved to:     {out_file}")
        else:
            print("Result:       No verified improvement found.")
        print("=" * 60)


if __name__ == "__main__":
    main()
