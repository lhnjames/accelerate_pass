"""
Identify the real computational hot function a kernel driver calls into,
for benchmarks where the shim's kernel_<name> entry point is a thin wrapper
around real work defined elsewhere -- e.g. SPEC mcf_r's kernel_mcf_r does
setup/IO then calls global_opt(), which has the actual while(new_arcs) {
primal_net_simplex(...); ... } solve loop. Without this, rewrite_source can
only ever edit the wrapper, which has nothing to do with the kernel's real
performance -- see docs/SPEC_mcf_r_build_status.md for how that manifested
(mcf_r's rewrite_source attempts kept "succeeding" at editing dead code).

Static analysis only -- this never touches IR or machine code (see the
constraint in docs/GENERIC_HARNESS_DESIGN.md and CLAUDE session notes: this
system only ever gets to a faster binary via source + compiler flags,
never by hand-editing .ll/.s output). It just decides *which C function*
rewrite_source's LLM should be shown and asked to change.
"""
from __future__ import annotations

import re
from typing import Optional

from tune_source import extract_kernel_function

_LOOP_RE = re.compile(r"\b(?:for|while)\s*\(")
_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(")
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

# Calls that mark a loop as (input) parsing/IO rather than algorithmic work
# -- e.g. SPEC mcf_r's read_min() parses the input file with a loop full of
# these and is textually *larger* than global_opt() (the actual solve
# dispatcher, which is a short "while (not converged) { solve_step(); }"),
# so a pure body-size or first-looping-callee heuristic picks the wrong one.
# There's no working profiler in this environment to settle it by real
# measurement (see the "perf_event_paranoid" note in select_hotspot_target's
# docstring), so this is a deliberately blunt static proxy: a candidate
# whose own loop calls any of these is presumed to be doing one-shot
# input handling, not the benchmark's repeated/hot computation.
_IO_HINT_CALLS = {
    "fscanf", "sscanf", "fgets", "fgetc", "getc", "getchar", "strtol",
    "strtod", "strtoul", "atoi", "atof", "atol", "scanf", "read", "getline",
}


def _has_loop(body: str) -> bool:
    return bool(_LOOP_RE.search(_strip_comments(body)))


def _direct_calls(body: str) -> list:
    seen, out = set(), []
    for m in _CALL_RE.finditer(_strip_comments(body)):
        name = m.group(1)
        if name in _SKIP_CALLEES or name in seen:
            continue
        seen.add(name)
        out.append(name)
    return out


def _score(body: str, direct_calls: list) -> float:
    """Static proxy for "is this candidate the real hot loop, not one-shot
    input parsing". See the _IO_HINT_CALLS comment for why this is needed
    and how blunt it necessarily is without a working profiler here."""
    score = 0.0
    if any(c in _IO_HINT_CALLS for c in direct_calls):
        score -= 100.0
    # Delegating to other locally-defined functions (not just doing its own
    # arithmetic inline) looks like a dispatcher over real algorithmic work
    # -- e.g. global_opt()'s while loop calling primal_net_simplex().
    score += 10.0 * len(direct_calls)
    score += 0.01 * len(body)  # minor tiebreaker
    return score


def select_hotspot_target(kernel_name: str, driver_text: str,
                          utils_text: "Optional[str]" = None,
                          max_hops: int = 3) -> dict:
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

    Selection: if kernel_<name> itself already contains a loop, it's
    presumably doing real work directly -- that's the common case
    (PolyBench/TSVC/CBench kernels) and matches today's behavior exactly.
    Otherwise (a thin wrapper), breadth-first walk its callees, collect
    *every* one that contains a loop within max_hops, and score them (see
    _score()) rather than taking the first match -- a plain "first looping
    callee wins" or "biggest body wins" heuristic picks the wrong function
    on real benchmarks: SPEC mcf_r's read_min() (one-shot input parsing,
    called before the solve) has a bigger, loop-bearing body than
    global_opt() (the actual repeated-solve dispatcher), and is textually
    first in the source too. There's no working profiler available in this
    environment to settle it by real measurement (perf_event_paranoid=4
    blocks perf record's call-graph sampling, and no VTune install either),
    so this stays a deliberately-labeled static proxy, not a claim of
    ground truth -- callers should treat `reason` as worth showing the LLM,
    not as a guarantee.
    """
    body, _, _ = extract_kernel_function(driver_text, kernel_name)
    if body is None:
        return {"name": kernel_name, "body": "", "in_utils": False,
                "reason": "kernel function not found"}

    if _has_loop(body):
        return {"name": kernel_name, "body": body, "in_utils": False,
                "reason": f"{kernel_name} itself contains a loop"}

    candidates = []  # (score, name, body, in_utils, hop)
    frontier = [kernel_name]
    visited = {kernel_name}
    for hop in range(max_hops):
        next_frontier = []
        for caller in frontier:
            caller_body, _, _ = extract_kernel_function(driver_text, caller)
            if caller_body is None and utils_text:
                caller_body, _, _ = extract_kernel_function(utils_text, caller)
            if caller_body is None:
                continue
            for callee in _direct_calls(caller_body):
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
                if _has_loop(callee_body):
                    callee_calls = _direct_calls(callee_body)
                    candidates.append((_score(callee_body, callee_calls),
                                       callee, callee_body, in_utils, hop + 1))
                else:
                    next_frontier.append(callee)
        frontier = next_frontier
        if not frontier:
            break

    if not candidates:
        return {"name": kernel_name, "body": body, "in_utils": False,
                "reason": (f"{kernel_name} has no loop and no looping callee "
                          f"found within {max_hops} hops -- falling back to the entry point")}

    candidates.sort(key=lambda c: c[0], reverse=True)
    score, name, sel_body, in_utils, hop = candidates[0]
    where = "utils/polybench.c" if in_utils else "the driver file"
    others = ", ".join(f"{c[1]} (score {c[0]:.0f})" for c in candidates[1:4])
    reason = (f"{kernel_name} has no loop of its own; {name} (reached via "
             f"{hop}-hop call chain, score {score:.0f}) does and is defined "
             f"in {where}, scored highest among looping candidates"
             + (f" (also considered: {others})" if others else ""))
    return {"name": name, "body": sel_body, "in_utils": in_utils, "reason": reason}
