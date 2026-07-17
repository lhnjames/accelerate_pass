"""
Canonical PolyBench directory-tree lookup.

Single source of truth for locating a PolyBench checkout's ``utilities/``
directory from a kernel source file. Shared by optimize.py, tune_param.py,
tune_source.py, src/compiler_manager.py, and src/datasets.py so the set of
recognized checkout directory names can't drift between them.
"""
from __future__ import annotations
from pathlib import Path
from typing import Optional

# Recognized PolyBench checkout root directory names.
# TSVC_shim / CBench_shim are synthetic PolyBench-shaped trees (see
# scripts/gen_tsvc_kernels.py / scripts/gen_cbench_kernels.py) that wrap
# TSVC loops and cBench hot functions in the same utilities/ + kernel_<x>()
# convention so the existing harness (compile/time/correctness) runs
# completely unmodified against them.
POLYBENCH_DIR_NAMES = frozenset({
    "PolyBenchC", "PolyBenchC_no_rag", "PolyBenchC_full",
    "PolyBenchC_no_vtune", "PolyBenchC_with_llm", "polybench",
    "TSVC_shim", "CBench_shim", "SPEC_shim",
})


def find_polybench_utilities(source_path: str) -> Optional[Path]:
    """Walk up from a kernel .c file to find its PolyBench utilities/ dir."""
    p = Path(source_path).resolve().parent
    while p != p.parent:
        if p.name in POLYBENCH_DIR_NAMES:
            utils = p / "utilities"
            return utils if utils.exists() else None
        p = p.parent
    return None
