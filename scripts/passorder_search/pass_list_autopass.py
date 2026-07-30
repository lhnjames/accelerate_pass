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
