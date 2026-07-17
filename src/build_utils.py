"""
Shared compile/run helpers for the flags (tune_param) and source-rewrite
(tune_source) optimization channels — both need to compile a PolyBench
kernel with clang and time the resulting binary the same way.
"""
from __future__ import annotations
import statistics
import subprocess
import time
from pathlib import Path
from typing import List, Optional, Sequence, Tuple, Union


def run_timing(bin_path: str, runs: int = 5, pin_cpu: "int | None" = None) -> float:
    """Run a compiled benchmark binary `runs` times (+1 warmup), return median ms.

    Pure external wall-clock timing (time.monotonic() wrapped around the
    subprocess) -- the binary's own stdout is not parsed for a self-reported
    time value. This means the target program does NOT need to call any
    instrumentation macros (e.g. PolyBench's polybench_start/stop/
    print_instruments) to be timeable; see docs/GENERIC_HARNESS_DESIGN.md.
    Kernels that still print a self-timed line (existing PolyBench kernels)
    keep working unmodified -- that line is just unused stdout now.
    """
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [bin_path]
    try:
        subprocess.run(cmd, capture_output=True, timeout=60)  # warmup, discarded
    except Exception:
        pass
    times = []
    for _ in range(runs):
        t0 = time.monotonic()
        try:
            res = subprocess.run(cmd, capture_output=True, timeout=60)
        except Exception:
            continue
        elapsed_ms = (time.monotonic() - t0) * 1000.0
        if res.returncode == 0:
            times.append(elapsed_ms)
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
    # gnu99, not strict c99: some CBench sources use BSD/POSIX typedefs
    # (e.g. libtiff's u_long) that only strict c99 hides. gnu99 is a strict
    # superset of c99 -- every kernel that compiled under c99 still does --
    # and matches what the CBench shim generator already test-compiles
    # with (scripts/gen_cbench_kernels.py's try_compile()), so a kernel
    # that's in the manifest at all is guaranteed compilable here too.
    cmd = ([clang_path, "-O3", "-std=gnu99"]
           + inc_flags + defines + list(sources) + ["-o", str(output_bin), "-lm"])
    if extra_flags:
        cmd.extend(extra_flags)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"compile timeout after {timeout}s"
