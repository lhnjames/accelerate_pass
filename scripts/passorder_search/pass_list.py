"""Canonical LLVM function-pass catalog for the AutoPass-style pass-order-
search baseline. AutoPass itself (arxiv 2606.20373) was dropped as a baseline
per explicit instruction, since we never got the actual repo -- this harness
is our own approximation of its core idea (searching over the ORDER of
compiler passes applied to a kernel, rather than changing source code or
tuning individual pass parameters) so the ablation study still has a
"pass-order-search" data point to compare against.

All names below are valid `opt -passes=<name>` new-pass-manager names on
LLVM 21 (verified via `opt-21 --print-passes` on the deployed toolchain).
mem2reg is NOT included here -- it is always prepended by measure_lib.py
before whatever order is chosen, since the frontend IR (-Xclang
-disable-llvm-passes) is still alloca-based and every other pass here is
far less effective (or a no-op) without SSA form first.
"""

CANONICAL_PASSES = [
    "sroa",
    "early-cse",
    "simplifycfg",
    "instcombine",
    "reassociate",
    "gvn",
    "loop-mssa(licm)",   # bare "licm" errors: "LICM requires MemorySSA (loop-mssa)"
    "loop-rotate",
    "simple-loop-unswitch",
    "indvars",
    "loop-idiom",
    "loop-deletion",
    "loop-unroll",
    "loop-vectorize",
    "slp-vectorizer",
    "correlated-propagation",
    "jump-threading",
    "sccp",
    "adce",
    "bdce",
    "dse",
    "memcpyopt",
]
