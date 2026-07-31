"""74-pass LLVM catalog for the AutoPass-faithful reproduction (arxiv 2606.20373).

The paper states its Reasoning Agent selects sequences (up to ~107 entries
long) from "74 LLVM optimization passes" but never enumerates the list. This
is our own curated 74-pass catalog assembled from `opt-21 --print-passes`
(new-PM function/loop transform passes only -- analyses, printers, debug and
target-specific passes excluded), chosen to be a superset of the smaller
22-pass CANONICAL_PASSES catalog used by the earlier (non-faithful) PO
harness. Any discrepancy with the paper's actual (undisclosed) list is
unavoidable -- this is disclosed here rather than silently implied to be
identical.

`mem2reg` is NOT included -- it is always prepended before whatever sequence
is chosen, since the frontend IR (-Xclang -disable-llvm-passes) is still
alloca-based and every pass below is far less effective (or a no-op)
without SSA form first.

"loop-mssa(licm)" is used instead of bare "licm" -- LICM requires MemorySSA
in LLVM 21's new pass manager and bare "licm" errors at runtime.
"""

CANONICAL_PASSES_74 = [
    "loop-mssa(licm)",
    "sroa",
    "early-cse",
    "simplifycfg",
    "instcombine",
    "aggressive-instcombine",
    "reassociate",
    "gvn",
    "gvn-hoist",
    "gvn-sink",
    "newgvn",
    "correlated-propagation",
    "jump-threading",
    "sccp",
    "adce",
    "bdce",
    "dce",
    "dse",
    "memcpyopt",
    "mldst-motion",
    "nary-reassociate",
    "slsr",
    "separate-const-offset-from-gep",
    "div-rem-pairs",
    "float2int",
    "typepromotion",
    "instsimplify",
    "tailcallelim",
    "mergereturn",
    "sink",
    "unify-loop-exits",
    "unreachableblockelim",
    "break-crit-edges",
    "lower-switch",
    "lower-constant-intrinsics",
    "lower-matrix-intrinsics",
    "infer-address-spaces",
    "infer-alignment",
    "libcalls-shrinkwrap",
    "partially-inline-libcalls",
    "interleaved-access",
    "load-store-vectorizer",
    "loop-vectorize",
    "slp-vectorizer",
    "scalarizer",
    "vector-combine",
    "speculative-execution",
    "safe-stack",
    "stack-protector",
    "constraint-elimination",
    "expand-reductions",
    "loop-distribute",
    "loop-fusion",
    "loop-load-elim",
    "loop-sink",
    "loop-data-prefetch",
    "loop-versioning",
    "loop-rotate",
    "simple-loop-unswitch",
    "indvars",
    "loop-idiom",
    "loop-idiom-vectorize",
    "loop-deletion",
    "loop-unroll",
    "loop-unroll-full",
    "loop-reduce",
    "loop-predication",
    "loop-simplifycfg",
    "loop-term-fold",
    "loop-mssa(lnicm)",  # like licm, lnicm requires the MemorySSA loop-adaptor wrapper
    "evl-iv-simplify",
    "loop-bound-split",
    "loop-versioning-licm",
    "loop-instsimplify",
]

assert len(CANONICAL_PASSES_74) == 74, len(CANONICAL_PASSES_74)

# NOTE: "codegenprepare" was tried and removed -- it requires a TargetMachine
# context (it's meant to run right before instruction selection inside llc,
# not as a standalone `opt` IR pass) and segfaults opt-21 unconditionally
# when included in any bare `-passes=` pipeline. Confirmed via a minimal
# `opt -passes=mem2reg,codegenprepare` repro on real kernel IR from this
# corpus. Replaced with "constraint-elimination" to keep the catalog at 74.


# ---------------------------------------------------------------------------
# Tunable pass PARAMETERS.
#
# AutoPass does not only reorder passes -- its Reasoning Agent "iteratively
# refines pass parameters" too, and the paper's QSort trace study (Sec 6.6)
# shows a concrete round-1 candidate setting
#     unroll_count=8, unroll_threshold=600, inline_threshold=800, slp_threshold=-5
# then walking those values back in round 2 after measuring a regression. An
# order-only search therefore under-reproduces the method.
#
# These are passed to `opt` as ordinary command-line flags (verified accepted
# by opt-21 alongside -passes=, e.g.
#     opt -passes='mem2reg,loop-rotate,loop-unroll' -unroll-threshold=600 ...).
# Every name below was checked against `opt-21 --help-hidden` on the deployed
# toolchain; the four the paper names explicitly are all present.
#
# Values are the suggested exploration set shown to the Reasoning Agent, not a
# hard constraint -- any integer is accepted for a known flag.
TUNABLE_PARAMS = {
    # loop unrolling (paper: unroll_count, unroll_threshold)
    "unroll-threshold":                    [50, 150, 300, 600, 1200],
    "unroll-count":                        [2, 4, 8, 16],
    "unroll-partial-threshold":            [50, 150, 300, 600],
    "unroll-max-count":                    [2, 4, 8, 16, 32],
    # inlining (paper: inline_threshold)
    "inline-threshold":                    [100, 225, 400, 800, 1600],
    "inlinehint-threshold":                [200, 325, 600, 1200],
    # SLP / loop vectorization (paper: slp_threshold)
    "slp-threshold":                       [-10, -5, 0, 5],
    "force-target-max-vector-interleave":  [1, 2, 4, 8],
    "vectorizer-min-trip-count":           [4, 8, 16],
    # scalar cleanup passes
    "licm-max-num-uses-traversed":         [8, 32, 128],
    "jump-threading-threshold":            [3, 6, 12, 24],
    "gvn-max-num-deps":                    [50, 100, 200],
    "loop-distribute-scev-check-threshold": [4, 8, 16, 32],
}
