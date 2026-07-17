"""
Dataset detection for COMET.

Only PolyBench is exercised by the active pipeline today. This module
detects the dataset family from a kernel source path so run logs/labels
can be tagged correctly.
"""

from __future__ import annotations
from pathlib import Path

from src.polybench_paths import find_polybench_utilities


def detect_dataset_type(src_path: str) -> str:
    """Return 'polybench' | 'tsvc' | 'cbench' | 'unknown'."""
    s = str(Path(src_path).resolve()).lower()
    if "tsvc" in s:
        return "tsvc"
    if "cbench" in s:
        return "cbench"
    if find_polybench_utilities(src_path) is not None:
        return "polybench"
    return "unknown"
