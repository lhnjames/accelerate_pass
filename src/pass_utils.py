"""
Utilities for building LLVM opt commands across pass manager variants.
"""
from pathlib import Path
from typing import List, Optional

# Passes that only work via legacy --flag syntax (not -passes=name).
LEGACY_FLAG_ONLY_PASSES: set = {"loop-interchange"}


def build_opt_command(
    opt_path: str,
    input_ir: "str | Path",
    output_ir: "str | Path",
    pass_sequence: List[str],
    extra_opt_args: Optional[List[str]] = None,
) -> List[str]:
    """
    Build an opt invocation for the given pass sequence.

    Most passes use the new pass manager:  opt -passes=a,b in.ll -o out.ll
    A small set (loop-interchange) require legacy syntax: opt --loop-interchange in.ll -o out.ll
    """
    extra_opt_args = extra_opt_args or []
    passes = [p for p in pass_sequence if p and p.strip()]

    def _base(p: str) -> str:
        return p.split()[0]

    def _extra_args(p: str) -> List[str]:
        parts = p.split()
        return parts[1:] if len(parts) > 1 else []

    if len(passes) == 1 and _base(passes[0]) in LEGACY_FLAG_ONLY_PASSES:
        return (
            [opt_path, f"--{_base(passes[0])}"]
            + _extra_args(passes[0])
            + extra_opt_args
            + [str(input_ir), "-o", str(output_ir)]
        )

    base_passes = [_base(p) for p in passes]
    pass_args   = [a for p in passes for a in _extra_args(p)]

    return (
        [opt_path, "-passes=" + ",".join(base_passes)]
        + pass_args
        + extra_opt_args
        + [str(input_ir), "-o", str(output_ir)]
    )
