"""
Shared compile/run helpers for the flags (tune_param) and source-rewrite
(tune_source) optimization channels — both need to compile a PolyBench
kernel with clang and time the resulting binary the same way.
"""
from __future__ import annotations
import statistics
import subprocess
from pathlib import Path
from typing import List, Optional, Sequence, Tuple, Union


def run_timing(bin_path: str, runs: int = 5, pin_cpu: "int | None" = None) -> float:
    """Run a compiled benchmark binary `runs` times (+1 warmup), return median ms."""
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [bin_path]
    try:
        subprocess.run(cmd, capture_output=True, timeout=60)
    except Exception:
        pass
    times = []
    for _ in range(runs):
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                                 errors="replace")
            if res.returncode == 0:
                out = res.stdout.strip() or res.stderr.strip()
                times.append(float(out.split()[-1]) * 1000.0)
        except Exception:
            pass
    return statistics.median(times) if times else -1.0


def compile_c(clang_path: str, sources: Sequence[str],
              include_dirs: Union[str, "Path", Sequence[Union[str, "Path"]]],
              defines: List[str], output_bin: str,
              extra_flags: Optional[List[str]] = None,
              timeout: Optional[int] = None) -> Tuple[bool, str]:
    """
    Compile C source(s) with -O3.
    extra_flags: additional flags (e.g. ["-mllvm", "-slp-threshold=-1"]) appended
                 after optimization flags. Used by source+param joint compilation.
    """
    if isinstance(include_dirs, (str, Path)):
        include_dirs = [include_dirs]
    inc_flags = [f"-I{d}" for d in include_dirs]
    cmd = ([clang_path, "-O3", "-std=c99"]
           + inc_flags + defines + list(sources) + ["-o", str(output_bin), "-lm"])
    if extra_flags:
        cmd.extend(extra_flags)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"compile timeout after {timeout}s"
