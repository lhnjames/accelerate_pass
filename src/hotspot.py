"""
Identify the real computational hot function a kernel driver calls into,
for benchmarks where the shim's kernel_<name> entry point is a thin wrapper
around real work defined elsewhere -- e.g. SPEC mcf_r's kernel_mcf_r does
setup/IO then calls global_opt(), which has the actual while(new_arcs) {
primal_net_simplex(...); ... } solve loop. Without this, rewrite_source can
only ever edit the wrapper, which has nothing to do with the kernel's real
performance -- see docs/SPEC_mcf_r_build_status.md for how that manifested
(mcf_r's rewrite_source attempts kept "succeeding" at editing dead code).

The hot function is not necessarily the one with a loop in its own body --
global_opt()'s while loop is just a convergence dispatcher; the arithmetic
that actually dominates runtime is inside primal_net_simplex(), which it
calls once per iteration. A candidate that's *called from inside* another
function's loop, and does real inline arithmetic rather than immediately
delegating further, is a stronger hotness signal than merely containing a
loop -- so this walks the whole reachable call graph (not stopping at the
first loop-bearing function) and scores every function found in it.

Static analysis only -- this never touches IR or machine code (see the
constraint in docs/GENERIC_HARNESS_DESIGN.md: this system only ever gets to
a faster binary via source + compiler flags, never by hand-editing .ll/.s
output). It just decides *which C function* rewrite_source's LLM should be
shown and asked to change.
"""
from __future__ import annotations

import re
from typing import Optional

from tune_source import extract_kernel_function

_LOOP_RE = re.compile(r"\b(?:for|while)\s*\(")
_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(")
_ARITH_RE = re.compile(r"(?<![-+*/%<>=!&|])[-+*/%](?!=?[-+*/%<>=!&|])|\[[^\[\]]*\]")
# Pointer-dereference and comparison ops -- the equivalent of "real inline
# computation" for pointer/struct-chasing code (linked lists, trees, graphs:
# SPEC mcf_r's simplex pivot, cBench's dijkstra/patricia) where the hot loop
# is dominated by `bea->head`, `iminus->pred`, `x != y` rather than `+`/`*`/
# array subscripts. Without this, _arith_density() systematically undervalues
# that whole workload class relative to array-heavy PolyBench-style code.
_PTR_BRANCH_RE = re.compile(r"->|[=!<>]=|(?<![<>=!])[<>](?!=)")
# Strips /* block */ and // line comments (not inside string/char literals --
# good enough for scanning C source for calls; a real license-header comment
# block like "Copyright (c) ... (ZIB)" would otherwise regex-match as a call
# to a function named "Copyright").
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def _strip_comments(text: str) -> str:
    return _COMMENT_RE.sub(" ", text)


# Calls to these are never a "real" hot-function target -- they're
# language/library plumbing, not the benchmark's own algorithm.
_SKIP_CALLEES = {
    "if", "for", "while", "switch", "return", "sizeof", "printf", "fprintf",
    "sprintf", "snprintf", "vprintf", "vfprintf", "malloc", "calloc",
    "realloc", "free", "memcpy", "memset", "memmove", "memcmp", "strcmp",
    "strncmp", "strcpy", "strncpy", "strcat", "strlen", "strdup", "fopen",
    "fclose", "fread", "fwrite", "fflush", "fgets", "fputs", "fputc",
    "fgetc", "exit", "abort", "assert", "perror",
}

# Calls that mark a function as (input) parsing/IO rather than algorithmic
# work -- e.g. SPEC mcf_r's read_min() parses the input file with a loop
# full of these and is textually *larger* than global_opt() (the actual
# solve dispatcher), so a pure body-size heuristic picks the wrong one.
# There's no working profiler in this environment to settle it by real
# measurement (perf_event_paranoid=4 blocks perf record's call-graph
# sampling, and no VTune install either), so this stays a deliberately
# blunt static proxy, not a claim of ground truth.
_IO_HINT_CALLS = {
    "fscanf", "sscanf", "fgets", "fgetc", "getc", "getchar", "strtol",
    "strtod", "strtoul", "atoi", "atof", "atol", "scanf", "read", "getline",
}

MAX_FUNCTIONS = 60  # hard cap on how much of the call graph to walk


def _has_loop(body: str) -> bool:
    return bool(_LOOP_RE.search(_strip_comments(body)))


def _loop_body_spans(text: str) -> list:
    """Char spans of each for/while loop's `{ ... }` body, via brace-counting
    from the loop header. Used to tell "called inside a loop" apart from
    "called once, outside any loop"."""
    spans = []
    for m in _LOOP_RE.finditer(text):
        brace_start = text.find("{", m.end())
        if brace_start == -1:
            continue
        depth, end = 0, -1
        for i in range(brace_start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        if end != -1:
            spans.append((brace_start, end))
    return spans


def _calls(body: str) -> list:
    """All distinct locally-relevant call targets in `body`, in order."""
    seen, out = set(), []
    for m in _CALL_RE.finditer(body):
        name = m.group(1)
        if name in _SKIP_CALLEES or name in seen:
            continue
        seen.add(name)
        out.append(name)
    return out


def _calls_inside_loops(body: str) -> set:
    """Subset of _calls(body) that occur textually inside one of this
    function's own loop bodies -- i.e. things this function runs
    repeatedly, as opposed to once during setup."""
    names = set()
    for start, end in _loop_body_spans(body):
        for m in _CALL_RE.finditer(body[start:end]):
            name = m.group(1)
            if name not in _SKIP_CALLEES:
                names.add(name)
    return names


def _arith_density(body: str) -> float:
    """Rough proxy for "does real inline computation" -- arithmetic
    operators, array-subscript expressions, pointer dereferences, and
    comparisons per character of body, after stripping comments (otherwise
    a comment full of ASCII-art or math notation could inflate this).
    Distinguishes a function that mostly just dispatches to other calls
    (low density) from one doing the actual work (high density). Pointer/
    comparison ops count at half weight of arithmetic/subscript ops -- see
    _PTR_BRANCH_RE."""
    body = _strip_comments(body)
    if not body:
        return 0.0
    arith = len(_ARITH_RE.findall(body))
    ptr = len(_PTR_BRANCH_RE.findall(body))
    return (arith + 0.5 * ptr) / max(len(body), 1)


def _self_loop_bonus(body: str) -> float:
    """Signal for "this function IS the hot loop", as opposed to "is called
    from inside someone else's loop" (see called_in_loop_by in _score).
    global_opt()'s `while(new_arcs) { primal_net_simplex(...); ... }` and
    master()'s `while(!opt) { primal_bea_mpp(...); ...branchy pivot work... }`
    both look identical under _has_loop() -- both have a top-level while
    loop occupying nearly their whole body -- but only one of them is
    actually doing the repeated work itself rather than delegating it
    further. Distinguish them by requiring the loop body to also be dense
    (by the same arith/pointer proxy as _arith_density): a pure dispatcher
    loop stays near-zero density no matter how much of the function it
    spans, while a real pivot/traversal loop doesn't.
    """
    stripped = _strip_comments(body)
    spans = _loop_body_spans(stripped)
    if not spans or not stripped:
        return 0.0
    # Union of loop-body character ranges (top-level loops rarely overlap,
    # but merge just in case of nesting) as a fraction of the whole function.
    covered = 0
    prev_end = -1
    for start, end in sorted(spans):
        start = max(start, prev_end)
        if end > start:
            covered += end - start
            prev_end = end
    coverage = covered / len(stripped)
    if coverage < 0.5:
        return 0.0
    loop_text = "".join(stripped[s:e] for s, e in spans)
    loop_density = (len(_ARITH_RE.findall(loop_text))
                    + 0.5 * len(_PTR_BRANCH_RE.findall(loop_text))) / max(len(loop_text), 1)
    if loop_density < 0.005:   # near-zero -- a thin call-dispatch loop, not real work
        return 0.0
    return 180.0               # comparable to the called_in_loop_by bonus


def _score(name: str, body: str, called_in_loop_by: set) -> float:
    """Static hotness proxy. Higher = more likely to be where runtime is
    actually spent. See module docstring for why "called from inside
    another function's loop" outweighs "has a loop of its own", and the
    _IO_HINT_CALLS comment for the input-parsing penalty. `body` need not
    be pre-stripped of comments -- this strips its own copy."""
    calls = _calls(_strip_comments(body))
    score = 0.0
    if name in called_in_loop_by:
        score += 200.0          # runs repeatedly -- the strongest signal available
    if _has_loop(body):
        score += 20.0
    score += _self_loop_bonus(body)          # IS the hot loop, not called from one
    score += 4000.0 * _arith_density(body)   # real inline computation
    if any(c in _IO_HINT_CALLS for c in calls):
        score -= 150.0           # one-shot input parsing, not the hot path
    score += 0.01 * len(body)    # tiebreaker only
    return score


def _build_and_score(kernel_name: str, driver_text: str,
                     utils_text: "Optional[str]", max_hops: int):
    """Shared graph-walk + scoring core for select_hotspot_target() and
    select_hotspot_targets() -- see select_hotspot_target's docstring
    (kept there since it's the primary/most-called entry point) for what
    this does and why. Returns (scored, graph_size) where scored is a
    list of (score, name, body, in_utils) sorted descending, or (None, 0)
    if kernel_name itself can't be found."""
    body, _, _ = extract_kernel_function(driver_text, kernel_name)
    if body is None:
        return None, 0, set()

    # Walk the whole reachable graph, recording every function's body/origin
    # and which functions get called from inside *someone's* loop.
    graph = {kernel_name: (body, False)}   # name -> (body, in_utils)
    called_in_loop = set()  # names invoked from inside any visited function's loop
    frontier = [kernel_name]
    visited = {kernel_name}
    graph[kernel_name] = (body, False)
    for _hop in range(max_hops):
        if len(graph) >= MAX_FUNCTIONS:
            break
        next_frontier = []
        for caller in frontier:
            caller_body, _ = graph[caller]
            called_in_loop |= _calls_inside_loops(_strip_comments(caller_body))
            for callee in _calls(_strip_comments(caller_body)):
                if callee in visited:
                    continue
                visited.add(callee)
                callee_body, _, _ = extract_kernel_function(driver_text, callee)
                in_utils = False
                if callee_body is None and utils_text:
                    callee_body, _, _ = extract_kernel_function(utils_text, callee)
                    in_utils = callee_body is not None
                if callee_body is None:
                    continue
                graph[callee] = (callee_body, in_utils)
                next_frontier.append(callee)
                if len(graph) >= MAX_FUNCTIONS:
                    break
            if len(graph) >= MAX_FUNCTIONS:
                break
        frontier = next_frontier
        if not frontier:
            break

    # kernel_name competes on equal footing with everything reachable from
    # it -- see select_hotspot_target's docstring for why a loop of its own
    # doesn't exempt it from that comparison.
    scored = sorted(
        ((_score(n, b, called_in_loop), n, b, u) for n, (b, u) in graph.items()),
        key=lambda c: c[0], reverse=True,
    )
    return scored, len(graph), called_in_loop


def select_hotspot_target(kernel_name: str, driver_text: str,
                          utils_text: "Optional[str]" = None,
                          max_hops: int = 4) -> dict:
    """
    Decide which function rewrite_source should actually show/target.

    Returns {"name", "body", "in_utils", "reason"}. `in_utils` is True if
    the selected function is defined in utils/polybench.c (the multi-file
    kernel's unity-built extra translation unit) rather than the driver
    file that holds kernel_<name> itself -- callers should treat that as
    informational for now (surface it in the prompt/diagnosis) rather than
    a rewrite target, since editing and re-linking a modified utils.c that
    *persists* across the agent's later steps needs the utils path itself
    to be a per-run mutable copy, which is a separate piece of plumbing.

    Selection: breadth-first walk the *entire* reachable call graph
    (bounded by max_hops and MAX_FUNCTIONS), starting from kernel_name, and
    score EVERY function reached -- including kernel_name itself -- the
    same way, picking whichever scores highest. An earlier version
    short-circuited here ("if kernel_name has a loop, it must be doing the
    real work, stop") -- true for the common case (PolyBench/TSVC/CBench
    kernels, whose loop body IS the dense array arithmetic) but wrong for
    e.g. SPEC lbm_r's kernel_lbm_r, whose `for` loop is a pure per-timestep
    dispatcher (LBM_handleInOutFlow/LBM_performStreamCollideTRT/
    LBM_swapGrids calls, no inline arithmetic of its own) around the
    function that does the actual lattice-Boltzmann compute -- the exact
    same "loop is a dispatcher, not the work" pattern as mcf_r's
    global_opt, just one level shallower. A loop by itself was never a
    reliable "real work happens here" signal; only _score() (arithmetic
    density + being called from inside someone else's loop) is, so every
    reachable function -- the entry included -- goes through it uniformly.
    A dense-loop kernel like a real PolyBench kernel still scores high on
    arithmetic density and simply wins on its own merits; no special case
    needed for that path either.

    See select_hotspot_targets() (plural) for the multi-function variant --
    mcf_r's primal_iminus/switch_arcs/markBaskets score within 4% of each
    other (the hot loop's cost is genuinely spread across several similarly-
    sized functions, not concentrated in one), so rewriting only the #1
    scorer caps achievable speedup at whatever that one function alone is
    worth. This function is unchanged/single-target for every existing
    caller; select_hotspot_targets is additive.
    """
    scored, graph_size, called_in_loop = _build_and_score(
        kernel_name, driver_text, utils_text, max_hops)
    if scored is None:
        return {"name": kernel_name, "body": "", "in_utils": False,
                "reason": "kernel function not found"}

    score, name, sel_body, in_utils = scored[0]
    others = ", ".join(f"{c[1]} (score {c[0]:.0f})" for c in scored[1:4])

    if name == kernel_name:
        body, _, _ = extract_kernel_function(driver_text, kernel_name)
        return {"name": kernel_name, "body": body, "in_utils": False,
                "reason": (f"{kernel_name} itself scored highest (score {score:.0f}) among "
                          f"{graph_size} reachable functions -- treating it as doing the "
                          f"real work directly"
                          + (f" (also considered: {others})" if others else ""))}

    where = "utils/polybench.c" if in_utils else "the driver file"
    signal = "called from inside a loop" if name in called_in_loop else (
        "contains a loop" if _has_loop(sel_body) else "highest arithmetic density found")
    reason = (f"{name} (score {score:.0f}, {signal}) scored higher than {kernel_name} itself "
             f"and is defined in {where}, across {graph_size} reachable functions"
             + (f" (also considered: {others})" if others else ""))
    return {"name": name, "body": sel_body, "in_utils": in_utils, "reason": reason}


def select_hotspot_targets(kernel_name: str, driver_text: str,
                           utils_text: "Optional[str]" = None,
                           max_hops: int = 4, max_targets: int = 6,
                           min_gap_pct: float = 8.0) -> list:
    """Plural variant of select_hotspot_target(): returns multiple targets
    instead of just the single best-scoring one, for kernels where the hot
    loop's cost is genuinely spread across several functions of comparable
    importance rather than concentrated in one -- see
    select_hotspot_target's docstring for the motivating mcf_r case.

    Cluster boundary is found from the score distribution itself, not a
    fixed cutoff percentage: among the top `max_targets` same-location
    candidates, compute each consecutive relative drop
    (score[i-1]-score[i])/score[i-1], and cut at whichever gap is LARGEST
    -- everything before that gap is "the cluster", everything after is a
    different tier. This adapts per kernel instead of assuming every
    kernel's cluster is bounded by the same percentage: reproduces both
    known cases from real score data with no tuning --
      mcf_r:  502, 486, 482, 386, ...  -> biggest gap is 482->386 (19.9%),
              cluster = {502, 486, 482}  (3 functions)
      lbm_r:  556, 540, 395, 354, ...  -> biggest gap is 540->395 (26.9%),
              cluster = {556, 540}       (2 functions)
    If the largest gap found is smaller than `min_gap_pct`, the scores are
    too flat to trust any cut point as a real category boundary (could just
    be noise among many similarly-unimportant functions) -- falls back to
    single-target rather than guessing a cluster size. Only functions
    sharing the SAME location (all in utils_text, or all in driver_text) as
    #1 are ever considered, since multi-target rewrite_source currently
    only supports splicing several spans within one shared text (see
    optimize.py's _splice_multi_spans).

    Returns a list of {"name", "body", "in_utils", "reason"} dicts, same
    shape as select_hotspot_target's return value, highest-scoring first
    (#1 always included even if kernel_name itself wins, in which case the
    list has exactly one entry -- self-hotspot kernels never get
    multi-target treatment, there is nothing else to cluster them with).
    """
    scored, graph_size, called_in_loop = _build_and_score(
        kernel_name, driver_text, utils_text, max_hops)
    if scored is None:
        return [{"name": kernel_name, "body": "", "in_utils": False,
                 "reason": "kernel function not found"}]

    top_score, top_name, top_body, top_in_utils = scored[0]
    if top_name == kernel_name:
        return [select_hotspot_target(kernel_name, driver_text, utils_text, max_hops)]

    same_loc = [c for c in scored if c[3] == top_in_utils][:max_targets]

    cut = 1  # default: just the top scorer, if no gap clears min_gap_pct
    best_gap = -1.0
    for i in range(1, len(same_loc)):
        prev_score = same_loc[i - 1][0]
        if prev_score <= 0:
            break
        gap_pct = (prev_score - same_loc[i][0]) / prev_score * 100.0
        if gap_pct > best_gap:
            best_gap = gap_pct
            cut = i
    if best_gap < min_gap_pct:
        cut = 1  # scores too flat to trust any cluster boundary

    picked = []
    for score, name, body, in_utils in same_loc[:cut]:
        where = "utils/polybench.c" if in_utils else "the driver file"
        signal = "called from inside a loop" if name in called_in_loop else (
            "contains a loop" if _has_loop(body) else "highest arithmetic density found")
        reason = (f"{name} (score {score:.0f}, {signal}) -- part of a {cut}-function cluster "
                 f"found by the biggest score gap ({best_gap:.0f}%) among the top "
                 f"{len(same_loc)} candidates, defined in {where}, among {graph_size} "
                 f"reachable functions -- joint-rewrite candidate")
        picked.append({"name": name, "body": body, "in_utils": in_utils, "reason": reason})
    return picked or [select_hotspot_target(kernel_name, driver_text, utils_text, max_hops)]
