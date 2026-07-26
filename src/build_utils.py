"""
Shared compile/run helpers for the flags (tune_param) and source-rewrite
(tune_source) optimization channels — both need to compile a PolyBench
kernel with clang and time the resulting binary the same way.
"""
from __future__ import annotations
import os
import re
import signal
import statistics
import subprocess
import time
from pathlib import Path
from typing import List, Optional, Sequence, Tuple, Union


# ── C vs C++ compiler selection ───────────────────────────────────────────────
#
# The whole pipeline historically only ever compiled C (.c) PolyBench-shaped
# kernels through a single hardcoded `clang_path`. Adding C++ SPEC benchmarks
# means some `sources` lists will contain .cpp/.cc/.cxx files, which the C
# frontend (plain `clang`) cannot compile, and which reject -std=gnu99
# outright ("invalid argument '-std=gnu99' not allowed with 'C++'"). Rather
# than requiring every one of compile_c()'s ~15+ call sites across
# optimize.py/tune_param.py/tune_source.py to be updated to thread a second
# "which compiler" decision through, the selection lives in ONE place
# (select_compiler(), used internally by compile_c() below) and is driven
# purely by the extensions actually present in `sources` -- every existing
# caller keeps working completely unchanged for C, and gets C++ support for
# free the moment its `sources` list contains a C++ file, with no signature
# changes required anywhere else.
_CXX_EXTENSIONS = {".cc", ".cp", ".cpp", ".cxx", ".c++", ".C", ".CPP"}

# Matches the two clang naming conventions actually seen on real systems:
# unversioned ("clang") and Debian/Ubuntu's versioned ("clang-11", "clang-21").
_CLANG_NAME_RE = re.compile(r"^clang(-(?P<ver>\d+))?$")


def derive_cxx_compiler_path(clang_path: str) -> Optional[str]:
    """Given a C clang binary path, derive the matching clang++ path using
    the standard naming convention (clang-N -> clang++-N, clang -> clang++).

    This is a name transform only -- it does not check the result exists on
    disk (callers do that, since "exists" has different fallback behavior
    depending on context). Returns None if `clang_path`'s basename doesn't
    match a recognized clang naming pattern (e.g. a custom-named binary),
    in which case the caller must fall back to an explicitly configured
    clang_cxx_path instead.
    """
    p = Path(clang_path)
    m = _CLANG_NAME_RE.match(p.name)
    if not m:
        return None
    ver = m.group("ver")
    cxx_name = f"clang++-{ver}" if ver else "clang++"
    return str(p.with_name(cxx_name))


def is_cxx_source(path: "str | Path") -> bool:
    """True if `path`'s extension marks it as a C++ (not C) source file."""
    return Path(path).suffix in _CXX_EXTENSIONS


class CompilerNotFoundError(RuntimeError):
    """Raised by select_compiler() when C++ sources are present but no
    working clang++ binary could be found (neither an explicit
    clang_cxx_path, a registered default, nor a derived one exists on disk)."""


# Registered default clang++ path (see set_default_cxx_compiler below). This
# exists so the EXPLICITLY CONFIGURED compiler.clang_cxx_path from
# configs/config.yaml wins over the naming-convention guess in
# derive_cxx_compiler_path(), without requiring every one of compile_c()'s
# ~15+ indirect call sites across optimize.py/tune_param.py/tune_source.py
# (extract_remarks_by_pass, get_ir_stats, extract_rich_remarks_yaml,
# extract_vectorization_remarks, _correctness_check, _detect_polybench_mode,
# compile_binary, ...) to each thread a clang_cxx parameter of their own
# through every intermediate function signature. Each entry point that loads
# config (optimize.py/tune_param.py/tune_source.py's main()) calls
# set_default_cxx_compiler(config.compiler.clang_cxx_path) exactly once at
# startup; every select_compiler() call anywhere in the process then
# consults it automatically. A caller that DOES have its own explicit
# clang_cxx_path in hand (e.g. compile_binary's clang_cxx kwarg) can still
# pass it directly, and an explicit non-None argument always wins over the
# registered default -- this is a default, not an override.
_default_cxx_path: Optional[str] = None


def set_default_cxx_compiler(clang_cxx_path: Optional[str]) -> None:
    """Register the process-wide default clang++ path, read once at startup
    from configs/config.yaml's compiler.clang_cxx_path. Pass None to clear
    it back to "no registered default" (falls through to derivation)."""
    global _default_cxx_path
    _default_cxx_path = clang_cxx_path


def get_default_cxx_compiler() -> Optional[str]:
    """Returns whatever set_default_cxx_compiler() last registered, or None
    if nothing has (yet). Exposed mainly for tests."""
    return _default_cxx_path


def select_compiler(sources: Sequence["str | Path"], clang_path: str,
                    clang_cxx_path: Optional[str] = None) -> Tuple[str, bool]:
    """Pick clang vs clang++ based on the extensions in `sources`.

    Returns (compiler_path, is_cxx). If none of `sources` is a C++ file,
    always returns (clang_path, False) unchanged -- this is the entire
    pipeline's existing behavior for every kernel compiled so far, and stays
    byte-for-byte identical for all of them.

    If any source IS a C++ file, resolution order is: (1) the explicit
    `clang_cxx_path` argument if given and it exists on disk, (2) the
    process-wide default registered via set_default_cxx_compiler() if set
    and it exists, (3) a path derived from `clang_path` via the standard
    clang-N -> clang++-N naming convention if THAT exists on disk, (4) raise
    CompilerNotFoundError with every attempted path named, so the caller's
    error message points directly at what to install or configure rather
    than failing with an opaque "file not found" from deep inside a
    subprocess call.
    """
    if not any(is_cxx_source(s) for s in sources):
        return clang_path, False

    if clang_cxx_path and Path(clang_cxx_path).exists():
        return clang_cxx_path, True

    if _default_cxx_path and Path(_default_cxx_path).exists():
        return _default_cxx_path, True

    derived = derive_cxx_compiler_path(clang_path)
    if derived and Path(derived).exists():
        return derived, True

    tried = [p for p in (clang_cxx_path, _default_cxx_path, derived) if p]
    raise CompilerNotFoundError(
        f"C++ source present but no clang++ binary found. Tried: {tried or '(none resolvable)'}. "
        f"Set compiler.clang_cxx_path in configs/config.yaml to an existing clang++ binary."
    )


def _trimmed_mean_ms(cmd: list, timeout: int, n: int = 4) -> float:
    """One ROBUST timing sample: execute `cmd` n times, drop the single
    highest and single lowest wall-clock reading, return the mean of what's
    left. Replaces "one raw execution = one sample" everywhere a program is
    timed (exploration screening AND final confirmation) so that a single
    unlucky/lucky OS scheduling blip can no longer masquerade as a real
    speedup or regression -- see docs on the exploratory-vs-confirmed
    speedup gap this was introduced to shrink."""
    times = []
    for _ in range(n):
        t0 = time.monotonic()
        try:
            res = subprocess.run(cmd, capture_output=True, timeout=timeout)
        except Exception:
            continue
        elapsed_ms = (time.monotonic() - t0) * 1000.0
        if res.returncode == 0:
            times.append(elapsed_ms)
    if len(times) < n - 1:
        # Too many failed/timed-out runs to safely drop both extremes.
        return statistics.mean(times) if times else -1.0
    times_sorted = sorted(times)
    trimmed = times_sorted[1:-1] if len(times_sorted) > 2 else times_sorted
    return statistics.mean(trimmed) if trimmed else -1.0


def run_timing(bin_path: str, runs: int = 5, pin_cpu: "int | None" = None,
               timeout_seconds: int = 600) -> float:
    """Run a compiled benchmark binary and return its timing in ms.

    Each of the `runs` samples is itself a ROBUST measurement: 4 raw
    executions (+1 warmup, discarded, upfront) with the single fastest and
    single slowest dropped and the remaining 2 averaged (see
    _trimmed_mean_ms). `run_timing` then returns the median across the
    `runs` such samples -- unchanged from before except that each sample
    is no longer a single noisy execution.

    Pure external wall-clock timing (time.monotonic() wrapped around the
    subprocess) -- the binary's own stdout is not parsed for a self-reported
    time value. This means the target program does NOT need to call any
    instrumentation macros (e.g. PolyBench's polybench_start/stop/
    print_instruments) to be timeable; see docs/GENERIC_HARNESS_DESIGN.md.
    Kernels that still print a self-timed line (existing PolyBench kernels)
    keep working unmodified -- that line is just unused stdout now.
    The timeout is configurable (default 600 s): large PolyBench solver
    instances can exceed the old 60 s hard limit during warmup.
    """
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [bin_path]
    timeout = max(1, int(timeout_seconds))
    try:
        subprocess.run(cmd, capture_output=True, timeout=timeout)  # warmup, discarded
    except Exception:
        pass
    samples = []
    for _ in range(max(1, runs)):
        m = _trimmed_mean_ms(cmd, timeout)
        if m > 0:
            samples.append(m)
    return statistics.median(samples) if samples else -1.0


def compile_c(clang_path: str, sources: Sequence[str],
              include_dirs: Union[str, "Path", Sequence[Union[str, "Path"]]],
              defines: List[str], output_bin: str,
              extra_flags: Optional[List[str]] = None,
              timeout: Optional[int] = None,
              clang_cxx_path: Optional[str] = None) -> Tuple[bool, str]:
    """
    Compile C (or C++, if `sources` contains a .cc/.cpp/.cxx file) source(s)
    with -O3. See select_compiler() in this module for how the compiler
    binary is chosen -- existing all-C callers are completely unaffected;
    `clang_cxx_path` only matters once a C++ source is actually present.

    extra_flags: additional flags (e.g. ["-mllvm", "-slp-threshold=-1"]) appended
                 after optimization flags. Used by source+param joint compilation.
    """
    if isinstance(include_dirs, (str, Path)):
        include_dirs = [include_dirs]
    inc_flags = [f"-I{d}" for d in include_dirs]

    try:
        compiler, is_cxx = select_compiler(sources, clang_path, clang_cxx_path)
    except CompilerNotFoundError as e:
        return False, str(e)

    if is_cxx:
        # Don't force a C standard on C++ sources -- -std=gnu99 is a C-only
        # flag and clang++ hard-errors on it ("not allowed with 'C++'").
        # Let clang++ use its own default C++ standard; a benchmark that
        # needs something more specific is a per-benchmark generation-time
        # concern (extra_flags), not a global one.
        std_flags = []
        # The clang++ DRIVER treats every input as C++ regardless of a .c
        # extension ("treating 'c' input as 'c++' when in C++ mode") unless
        # told otherwise -- confirmed by an actual mixed-source build
        # failing to link ("undefined reference") because the .c file's own
        # definition got C++ name-mangled while the .cpp caller's extern "C"
        # declaration expected the unmangled C symbol. Real C++ SPEC
        # benchmarks mix .c helper files with .cpp files, so language must
        # be pinned per-source explicitly via -x, not left to the driver's
        # extension-based guess (which only applies under the plain `clang`
        # C driver, not `clang++`).
        source_args: List[str] = []
        _last_lang: Optional[str] = None
        for s in sources:
            lang = "c++" if is_cxx_source(s) else "c"
            if lang != _last_lang:
                source_args += ["-x", lang]
                _last_lang = lang
            source_args.append(str(s))
    else:
        # gnu99, not strict c99: some CBench sources use BSD/POSIX typedefs
        # (e.g. libtiff's u_long) that only strict c99 hides. gnu99 is a strict
        # superset of c99 -- every kernel that compiled under c99 still does --
        # and matches what the CBench shim generator already test-compiles
        # with (scripts/gen_cbench_kernels.py's try_compile()), so a kernel
        # that's in the manifest at all is guaranteed compilable here too.
        std_flags = ["-std=gnu99"]
        source_args = list(sources)

    cmd = ([compiler, "-O3"] + std_flags
           + inc_flags + defines + source_args + ["-o", str(output_bin), "-lm"])
    if extra_flags:
        cmd.extend(extra_flags)
    # `clang` (the driver) forks a `-cc1` backend subprocess to actually do the
    # work; subprocess.run(..., timeout=N) on timeout only kills the driver
    # (its direct child), leaving a hung -cc1 GRANDCHILD running orphaned --
    # observed live, twice, burning a full CPU core for 30-50+ minutes each
    # time (LLVM 11's SLP vectorizer pathologically slow on certain flag/
    # kernel combos, e.g. -slp-min-tree-size=0/1 on SPEC lbm_r's polybench.c).
    # start_new_session=True puts the whole clang+cc1 tree in its own process
    # group so a timeout can kill all of it via killpg, not just the driver.
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, errors="replace", start_new_session=True)
    except Exception as e:
        return False, str(e)
    try:
        _out, err = proc.communicate(timeout=timeout)
        return proc.returncode == 0, err
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
        return False, f"compile timeout after {timeout}s"
