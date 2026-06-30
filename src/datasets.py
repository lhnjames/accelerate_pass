"""
Dataset abstractions for COMET: PolyBench, TSVC, cBench.

Each adapter knows how to:
  - Detect whether a source path belongs to this dataset
  - Compile the program (dataset-specific flags/helper files)
  - Validate correctness (dataset-specific mechanism)
  - Parse timing output
"""

from __future__ import annotations
import os
import re
import subprocess
import tempfile
import shutil
from pathlib import Path
from typing import List, Optional, Tuple, Dict


# ── Dataset roots (canonical locations) ────────────────────────────────────
DATASETS_ROOT   = Path("/home/hanning/accelerate/datasets")
POLYBENCH_ROOT  = DATASETS_ROOT / "polybench"
TSVC_ROOT       = DATASETS_ROOT / "tsvc"
CBENCH_ROOT     = DATASETS_ROOT / "cbench"


# ── Detection ───────────────────────────────────────────────────────────────

def detect_dataset_type(src_path: str) -> str:
    """Return 'polybench' | 'tsvc' | 'cbench' | 'unknown'."""
    p = Path(src_path).resolve()
    s = str(p)
    if "TSVC" in s or "tsvc" in s.lower():
        return "tsvc"
    if "cBench" in s or "cbench" in s.lower():
        return "cbench"
    # PolyBench: directory contains polybench.h or utilities/polybench.c ancestor
    for parent in p.parents:
        if (parent / "utilities" / "polybench.c").exists():
            return "polybench"
        if (parent / "polybench.c").exists():
            return "polybench"
    return "unknown"


# ── PolyBench ───────────────────────────────────────────────────────────────

def polybench_utilities(src_path: str) -> Optional[Path]:
    """Walk up from src to find the PolyBench utilities/ directory."""
    p = Path(src_path).resolve()
    for parent in p.parents:
        utils = parent / "utilities"
        if (utils / "polybench.c").exists():
            return utils
    return None


def compile_polybench(clang: str, src: str, extra_flags: List[str],
                      out_bin: str, dataset_flag: str = "LARGE_DATASET",
                      dump_arrays: bool = False,
                      timeout: int = 60) -> Tuple[bool, str]:
    """Compile a PolyBench kernel with polybench.c support."""
    utils = polybench_utilities(src)
    if utils is None:
        return False, "Cannot find PolyBench utilities/ directory"
    polybench_c = utils / "polybench.c"
    source_dir  = Path(src).parent
    include_dirs = [str(utils), str(source_dir)]
    defs = [f"-D{dataset_flag}"]
    if dump_arrays:
        defs.append("-DPOLYBENCH_DUMP_ARRAYS")
    else:
        defs.append("-DPOLYBENCH_TIME")
    cmd = [clang, "-O3", "-march=native", "-std=c99"]
    for d in include_dirs:
        cmd += ["-I", d]
    cmd += defs + extra_flags + [src, str(polybench_c), "-o", out_bin, "-lm"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"compile timeout after {timeout}s"


def validate_polybench(clang: str, opt_src: str, ref_src: str,
                       tmpdir: str, tag: str,
                       epsilon: float = 1e-4,
                       timeout: int = 45) -> Tuple[bool, str]:
    """Validate PolyBench kernel via POLYBENCH_DUMP_ARRAYS numerical comparison."""
    for ds in ("SMALL_DATASET", "MINI_DATASET"):
        ref_bin = os.path.join(tmpdir, f"{tag}_ref_{ds}")
        opt_bin = os.path.join(tmpdir, f"{tag}_opt_{ds}")
        ok, err = compile_polybench(clang, ref_src, [], ref_bin,
                                    dataset_flag=ds, dump_arrays=True, timeout=30)
        if not ok:
            continue
        ok2, err2 = compile_polybench(clang, opt_src, [], opt_bin,
                                      dataset_flag=ds, dump_arrays=True, timeout=30)
        if not ok2:
            return False, f"optimized compile failed ({ds}): {err2[:200]}"
        try:
            ref_out = subprocess.run([ref_bin], capture_output=True, text=True, timeout=timeout)
            opt_out = subprocess.run([opt_bin], capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, f"runtime timeout ({ds})"
        ref_nums = _extract_numbers(ref_out.stdout + ref_out.stderr)
        opt_nums = _extract_numbers(opt_out.stdout + opt_out.stderr)
        if not ref_nums:
            return False, f"no numeric output from reference ({ds})"
        ok3, msg = _compare_numbers(ref_nums, opt_nums, epsilon)
        if not ok3:
            return False, f"[{ds}] {msg}"
        return True, ""
    return False, "could not compile with SMALL_DATASET or MINI_DATASET"


# ── TSVC ────────────────────────────────────────────────────────────────────

def tsvc_src_dir() -> Path:
    return TSVC_ROOT / "src"


def compile_tsvc(clang: str, extra_flags: List[str],
                 out_bin: str, timeout: int = 60) -> Tuple[bool, str]:
    """Compile the full TSVC suite (tsvc.c + common.c + dummy.c)."""
    src_dir = tsvc_src_dir()
    sources = [str(src_dir / f) for f in ("tsvc.c", "common.c", "dummy.c")]
    cmd = ([clang, "-O3", "-march=native", "-std=c99"]
           + ["-I", str(src_dir)]
           + extra_flags
           + sources
           + ["-o", out_bin, "-lm"])
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"compile timeout after {timeout}s"


def run_tsvc(binary: str, timeout: int = 120) -> Dict[str, float]:
    """Run TSVC binary, return dict of {loop_name: checksum}."""
    try:
        r = subprocess.run([binary], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {}
    checksums: Dict[str, float] = {}
    for line in r.stdout.splitlines():
        # Format: " s000\t     0.002\t512066944.000000"
        parts = line.split()
        if len(parts) >= 3:
            try:
                checksums[parts[0].strip()] = float(parts[-1])
            except ValueError:
                pass
    return checksums


def validate_tsvc(clang: str, extra_flags: List[str],
                  tmpdir: str, tag: str,
                  epsilon: float = 1e-4,
                  timeout: int = 120) -> Tuple[bool, str]:
    """Validate TSVC by comparing per-loop checksums of baseline vs optimized builds."""
    ref_bin = os.path.join(tmpdir, f"{tag}_tsvc_ref")
    opt_bin = os.path.join(tmpdir, f"{tag}_tsvc_opt")
    ok, err = compile_tsvc(clang, [], ref_bin, timeout=60)
    if not ok:
        return False, f"TSVC ref compile failed: {err[:200]}"
    ok, err = compile_tsvc(clang, extra_flags, opt_bin, timeout=60)
    if not ok:
        return False, f"TSVC opt compile failed: {err[:200]}"
    ref_cs = run_tsvc(ref_bin, timeout=timeout)
    opt_cs = run_tsvc(opt_bin, timeout=timeout)
    if not ref_cs:
        return False, "TSVC reference produced no checksums"
    failures = []
    for name, ref_val in ref_cs.items():
        opt_val = opt_cs.get(name)
        if opt_val is None:
            failures.append(f"{name}: missing in optimized output")
            continue
        denom = max(abs(ref_val), 1.0)
        if abs(opt_val - ref_val) / denom > epsilon:
            failures.append(f"{name}: ref={ref_val:.6g} opt={opt_val:.6g}")
    if failures:
        return False, "checksum mismatch: " + "; ".join(failures[:5])
    return True, ""


def tsvc_total_time(binary: str, timeout: int = 120) -> float:
    """Return total wall-clock time across all TSVC loops (ms)."""
    try:
        import time as _time
        t0 = _time.monotonic()
        r = subprocess.run([binary], capture_output=True, text=True, timeout=timeout)
        wall = (_time.monotonic() - t0) * 1000
        # Also sum per-loop times from stdout
        loop_sum = 0.0
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                try:
                    loop_sum += float(parts[1]) * 1000  # seconds → ms
                except ValueError:
                    pass
        # Prefer per-loop sum (more stable), fall back to wall time
        return loop_sum if loop_sum > 0 else wall
    except subprocess.TimeoutExpired:
        return -1.0


# ── cBench ──────────────────────────────────────────────────────────────────

def cbench_benchmark_dir(src_path: str) -> Optional[Path]:
    """Walk up from src_path to find a cBench benchmark directory (has _ccc_info_datasets)."""
    p = Path(src_path).resolve()
    for parent in [p.parent] + list(p.parents):
        if (parent / "_ccc_info_datasets").exists():
            return parent
        if (parent / "src" / "_ccc_info_datasets").exists():
            return parent / "src"
    return None


def cbench_source_files(bench_src_dir: Path) -> List[str]:
    """Return list of .c source files for a cBench benchmark (excludes loop-wrap.c)."""
    files = [str(f) for f in bench_src_dir.glob("*.c")
             if f.name != "loop-wrap.c"]
    return sorted(files)


def cbench_dataset_cmd(bench_src_dir: Path, dataset_num: int = 1) -> Optional[str]:
    """Parse _ccc_info_datasets and return the command args for dataset N."""
    info_file = bench_src_dir / "_ccc_info_datasets"
    if not info_file.exists():
        return None
    text = info_file.read_text()
    blocks = re.split(r"^=====\s*$", text, flags=re.MULTILINE)
    for block in blocks:
        lines = [l.strip() for l in block.strip().splitlines() if l.strip()]
        if not lines:
            continue
        try:
            if int(lines[0]) == dataset_num and len(lines) >= 2:
                return lines[1]  # command args
        except ValueError:
            pass
    return None


def cbench_reference_output_dir(bench_src_dir: Path) -> Path:
    """Directory where cBench reference outputs are stored."""
    return bench_src_dir / "_comet_ref_outputs"


def compile_cbench(clang: str, bench_src_dir: Path,
                   extra_flags: List[str], out_bin: str,
                   timeout: int = 60) -> Tuple[bool, str]:
    """Compile a cBench benchmark (with loop-wrap.c for timing wrapper)."""
    sources = cbench_source_files(bench_src_dir)
    if not sources:
        return False, f"No source files in {bench_src_dir}"
    loop_wrap = bench_src_dir / "loop-wrap.c"
    if loop_wrap.exists():
        sources.append(str(loop_wrap))
    cmd = ([clang, "-O3", "-march=native", "-std=c99",
            "-I", str(bench_src_dir)]
           + extra_flags + sources + ["-o", out_bin, "-lm"])
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"compile timeout after {timeout}s"


def _cbench_expand_cmd(bench_src_dir: Path, cmd_args: str,
                       run_dir: str) -> Tuple[List[str], Optional[str]]:
    """
    Parse cBench cmd_args string into (argv_list, stdout_redirect_path).
    Handles shell redirect "> filename" by extracting it from the arg list.
    Expands ../../data/... paths relative to the cBench root.
    """
    bench_root = bench_src_dir.parent.parent  # cBench/
    tokens = cmd_args.split()
    expanded: List[str] = []
    stdout_redir: Optional[str] = None
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok == ">":
            # Shell redirect: next token is the file name
            if i + 1 < len(tokens):
                stdout_redir = os.path.join(run_dir, tokens[i + 1])
                i += 2
            else:
                i += 1
        elif tok.startswith("../../"):
            expanded.append(str(bench_root / tok[6:]))
            i += 1
        else:
            expanded.append(tok)
            i += 1
    return expanded, stdout_redir


def _cbench_run(binary: str, bench_src_dir: Path,
                cmd_args: str, run_dir: str,
                n_loops: int = 1, timeout: int = 60) -> Tuple[bool, str]:
    """Run a cBench binary in run_dir with cmd_args, returns (success, error_msg).

    Correctly handles shell redirects (> file) in cmd_args.
    """
    finfo = os.path.join(run_dir, "_finfo_dataset")
    with open(finfo, "w") as f:
        f.write(str(n_loops))
    argv, stdout_redir = _cbench_expand_cmd(bench_src_dir, cmd_args, run_dir)
    full_cmd = [binary] + argv
    try:
        if stdout_redir:
            with open(stdout_redir, "w") as stdout_f:
                r = subprocess.run(
                    full_cmd, stdout=stdout_f, stderr=subprocess.PIPE,
                    timeout=timeout, cwd=run_dir, errors="replace",
                )
        else:
            r = subprocess.run(
                full_cmd, capture_output=True, text=True,
                timeout=timeout, cwd=run_dir, errors="replace",
            )
        return r.returncode == 0, r.stderr
    except subprocess.TimeoutExpired:
        return False, f"runtime timeout after {timeout}s"


def cbench_generate_reference(clang: str, bench_src_dir: Path,
                               dataset_num: int = 1,
                               timeout: int = 60) -> Tuple[bool, str]:
    """
    Compile baseline -O3 build, run it, and store output files as reference.
    Must be called before validate_cbench can work.
    """
    ref_dir = cbench_reference_output_dir(bench_src_dir)
    ref_dir.mkdir(exist_ok=True)
    cmd_args = cbench_dataset_cmd(bench_src_dir, dataset_num)
    if cmd_args is None:
        return False, f"No dataset {dataset_num} in _ccc_info_datasets"
    with tempfile.TemporaryDirectory() as tmpdir:
        ref_bin = os.path.join(tmpdir, "cbench_ref")
        ok, err = compile_cbench(clang, bench_src_dir, [], ref_bin, timeout)
        if not ok:
            return False, f"ref compile failed: {err[:300]}"
        run_dir = os.path.join(tmpdir, "run")
        os.makedirs(run_dir)
        ok, err = _cbench_run(ref_bin, bench_src_dir, cmd_args, run_dir, timeout=timeout)
        if not ok:
            return False, f"ref run failed: {err[:200]}"
        # Copy output files to ref_dir
        _collect_cbench_outputs(run_dir, ref_dir, dataset_num)
    return True, ""


def _collect_cbench_outputs(run_dir: str, dest: Path, dataset_num: int):
    """Copy all non-trivial output files from run_dir to dest."""
    _SKIP_PREFIXES = ("_finfo",)
    for fname in os.listdir(run_dir):
        if any(fname.startswith(p) for p in _SKIP_PREFIXES):
            continue
        src_f = os.path.join(run_dir, fname)
        if os.path.isfile(src_f) and os.path.getsize(src_f) > 0:
            shutil.copy2(src_f, str(dest / f"ds{dataset_num}_{fname}"))


def validate_cbench(clang: str, bench_src_dir: Path,
                    extra_flags: List[str],
                    dataset_num: int = 1,
                    timeout: int = 60) -> Tuple[bool, str]:
    """
    Validate cBench benchmark: run optimized build and compare output files
    against stored reference (generated by cbench_generate_reference).
    """
    ref_dir = cbench_reference_output_dir(bench_src_dir)
    if not ref_dir.exists() or not any(ref_dir.iterdir()):
        # Auto-generate reference on first call
        ok, err = cbench_generate_reference(clang, bench_src_dir, dataset_num, timeout)
        if not ok:
            return False, f"reference generation failed: {err}"

    cmd_args = cbench_dataset_cmd(bench_src_dir, dataset_num)
    if cmd_args is None:
        return False, "no dataset info"

    with tempfile.TemporaryDirectory() as tmpdir:
        opt_bin = os.path.join(tmpdir, "cbench_opt")
        ok, err = compile_cbench(clang, bench_src_dir, extra_flags, opt_bin, timeout)
        if not ok:
            return False, f"opt compile failed: {err[:300]}"
        run_dir = os.path.join(tmpdir, "run")
        os.makedirs(run_dir)
        ok, err = _cbench_run(opt_bin, bench_src_dir, cmd_args, run_dir, timeout=timeout)
        if not ok:
            return False, f"opt run failed: {err[:200]}"
        # Compare output files
        prefix = f"ds{dataset_num}_"
        for ref_fname in ref_dir.iterdir():
            if not ref_fname.name.startswith(prefix):
                continue
            out_fname = ref_fname.name[len(prefix):]
            opt_file  = os.path.join(run_dir, out_fname)
            if not os.path.exists(opt_file):
                return False, f"optimized output missing: {out_fname}"
            ref_bytes = ref_fname.read_bytes()
            opt_bytes = Path(opt_file).read_bytes()
            if ref_bytes != opt_bytes:
                # Numeric tolerance fallback
                ref_nums = _extract_numbers(ref_bytes.decode("latin-1", errors="replace"))
                opt_nums = _extract_numbers(opt_bytes.decode("latin-1", errors="replace"))
                if ref_nums and opt_nums:
                    ok3, msg = _compare_numbers(ref_nums, opt_nums, 1e-4)
                    if not ok3:
                        return False, f"{out_fname}: {msg}"
                else:
                    if ref_bytes != opt_bytes:
                        return False, f"{out_fname}: binary output differs"
    return True, ""


def cbench_timing(binary: str, bench_src_dir: Path,
                  cmd_args: str, n_loops: int = 5,
                  timeout: int = 120) -> float:
    """Return median execution time (ms) for a cBench binary."""
    import time as _time
    times = []
    with tempfile.TemporaryDirectory() as tmpdir:
        finfo = os.path.join(tmpdir, "_finfo_dataset")
        with open(finfo, "w") as f:
            f.write("1")  # 1 iteration per run call
        bench_root = bench_src_dir.parent.parent
        expanded = []
        for tok in cmd_args.split():
            if tok.startswith("../../"):
                expanded.append(str(bench_root / tok[6:]))
            else:
                # output files go to tmpdir
                expanded.append(os.path.join(tmpdir, tok) if not tok.startswith("-") and "." in tok and not os.sep in tok else tok)
        for _ in range(n_loops):
            t0 = _time.monotonic()
            try:
                subprocess.run([binary] + expanded,
                               capture_output=True, cwd=tmpdir, timeout=timeout)
            except subprocess.TimeoutExpired:
                return -1.0
            times.append((_time.monotonic() - t0) * 1000)
    if not times:
        return -1.0
    times.sort()
    return times[len(times) // 2]


# ── Shared numeric utilities ─────────────────────────────────────────────────

_NUM_RE = re.compile(r"[-+]?\d+\.?\d*(?:[eE][-+]?\d+)?")

def _extract_numbers(text: str) -> List[float]:
    return [float(m) for m in _NUM_RE.findall(text)]


def _compare_numbers(ref: List[float], opt: List[float],
                     epsilon: float) -> Tuple[bool, str]:
    if len(ref) != len(opt):
        return False, f"output length mismatch: ref={len(ref)} opt={len(opt)}"
    for i, (r, o) in enumerate(zip(ref, opt)):
        denom = max(abs(r), 1.0)
        if abs(r - o) / denom > epsilon:
            return False, f"value[{i}]: ref={r:.6g} opt={o:.6g} rel_err={abs(r-o)/denom:.2e}"
    return True, ""
