"""OpenEvolve evaluator for one PolyBench kernel — matched to COMET's protocol.

OpenEvolve maximizes `combined_score`.  Here that score is the measured -O3
speedup of the evolved kernel over the ORIGINAL -O3 baseline, but ONLY if the
evolved kernel is numerically correct; an incorrect kernel scores 0 no matter
how fast it runs.  This is the same discipline COMET applies (a wrong-but-fast
rewrite is never a win) and is what makes the two systems comparable rather
than one being allowed to cheat.

Measurement is deliberately identical to COMET's own harness:
  * same compiler (clang-21), same -O3, same LARGE_DATASET,
  * correctness via -DPOLYBENCH_DUMP_ARRAYS full-array comparison against the
    untouched reference source (numeric tolerance, or bit-identical),
  * timing via external wall-clock (never the program's own stdout), CPU-pinned,
    median of a few runs for the in-loop fitness (the driver re-confirms the
    final winner at n=5, exactly as COMET does).

Per-program paths/parameters come from eval_config.json sitting next to this
file (one self-contained directory per program), so a single template serves
every kernel without edits.
"""
import json
import os
import re
import statistics
import subprocess
import tempfile
from pathlib import Path

_HERE = Path(os.path.abspath(os.path.dirname(__file__)))
_CFG = json.loads((_HERE / "eval_config.json").read_text())
_BASELINE_CACHE = _HERE / ".baseline_ms.json"

CLANG = _CFG["clang"]
UTIL_DIR = _CFG["utilities_dir"]
SRC_INC = _CFG["source_include_dir"]
REF_C = _CFG["reference_c"]
POLYBENCH_C = _CFG["polybench_c"]
PIN_CPU = _CFG.get("pin_cpu")
DATASET = _CFG.get("dataset_define", "LARGE_DATASET")
FITNESS_RUNS = int(_CFG.get("fitness_runs", 3))
TIMEOUT = int(_CFG.get("compile_timeout", 300))
RUN_TIMEOUT = int(_CFG.get("run_timeout", 300))
TOL = float(_CFG.get("rel_tol", 1e-6))


def _compile(src_c, out_bin, extra_defines):
    cmd = [CLANG, "-O3", "-I", UTIL_DIR, "-I", SRC_INC,
           f"-D{DATASET}", *extra_defines,
           src_c, POLYBENCH_C, "-o", str(out_bin), "-lm"]
    p = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT)
    return p.returncode == 0, p.stderr.decode("utf-8", "replace")


def _run_dump(bin_path):
    """Run with DUMP_ARRAYS; PolyBench dumps to stderr."""
    cmd = ([("taskset")] + (["-c", str(PIN_CPU)] if PIN_CPU is not None else [])
           + [str(bin_path)]) if PIN_CPU is not None else [str(bin_path)]
    p = subprocess.run(cmd, capture_output=True, timeout=RUN_TIMEOUT)
    return p.stderr.decode("utf-8", "replace")


def _time_once(bin_path):
    """External wall-clock ms for one run (matches COMET's _single_shot_ms_external)."""
    cmd = (["taskset", "-c", str(PIN_CPU)] if PIN_CPU is not None else []) + [str(bin_path)]
    import time as _t
    t0 = _t.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=RUN_TIMEOUT)
    except Exception:
        return -1.0
    dt = (_t.monotonic() - t0) * 1000.0
    return dt if p.returncode == 0 else -1.0


def _nums(text):
    return [float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)]


def _baseline_ms(workdir):
    if _BASELINE_CACHE.exists():
        return json.loads(_BASELINE_CACHE.read_text())["baseline_ms"]
    ref_bin = workdir / "baseline_ref"
    ok, err = _compile(REF_C, ref_bin, ["-DPOLYBENCH_TIME"])
    if not ok:
        raise RuntimeError(f"baseline compile failed: {err[:200]}")
    _time_once(ref_bin)  # warmup
    samples = sorted(t for t in (_time_once(ref_bin) for _ in range(5)) if t > 0)
    base = statistics.median(samples)
    _BASELINE_CACHE.write_text(json.dumps({"baseline_ms": base}))
    return base


def evaluate(program_path):
    """Return metrics dict; combined_score is speedup-if-correct-else-0."""
    with tempfile.TemporaryDirectory() as td:
        wd = Path(td)
        try:
            baseline = _baseline_ms(wd)
        except Exception as exc:
            return {"combined_score": 0.0, "correct": 0.0, "error": f"baseline: {exc}"}

        # ── correctness: full-array dump comparison vs untouched reference ──
        ref_bin = wd / "ref_dump"
        cand_dump_bin = wd / "cand_dump"
        ok_r, _ = _compile(REF_C, ref_bin, ["-DPOLYBENCH_DUMP_ARRAYS"])
        ok_c, cerr = _compile(program_path, cand_dump_bin, ["-DPOLYBENCH_DUMP_ARRAYS"])
        if not ok_c:
            return {"combined_score": 0.0, "correct": 0.0,
                    "compile_ok": 0.0, "error": f"compile: {cerr[:200]}"}
        if not ok_r:
            return {"combined_score": 0.0, "correct": 0.0, "error": "ref compile"}
        ref_out = _nums(_run_dump(ref_bin))
        cand_out = _nums(_run_dump(cand_dump_bin))
        if not ref_out or len(ref_out) != len(cand_out):
            return {"combined_score": 0.0, "correct": 0.0, "compile_ok": 1.0,
                    "error": f"dump count {len(ref_out)} vs {len(cand_out)}"}
        max_rel = max(abs(a - b) / max(abs(a), 1e-30)
                      for a, b in zip(ref_out, cand_out))
        if max_rel > TOL:
            return {"combined_score": 0.0, "correct": 0.0, "compile_ok": 1.0,
                    "max_rel_err": max_rel, "error": "numeric mismatch"}

        # ── timing: candidate at -O3, external wall clock, CPU-pinned ──
        cand_time_bin = wd / "cand_time"
        ok_t, terr = _compile(program_path, cand_time_bin, ["-DPOLYBENCH_TIME"])
        if not ok_t:
            return {"combined_score": 0.0, "correct": 1.0, "error": f"time-compile: {terr[:200]}"}
        _time_once(cand_time_bin)  # warmup
        samples = sorted(t for t in (_time_once(cand_time_bin)
                                     for _ in range(FITNESS_RUNS)) if t > 0)
        if not samples:
            return {"combined_score": 0.0, "correct": 1.0, "error": "candidate run failed"}
        cand_ms = statistics.median(samples)
        speedup = baseline / cand_ms
        return {
            "combined_score": speedup,   # OpenEvolve maximizes this
            "speedup": speedup,
            "correct": 1.0,
            "compile_ok": 1.0,
            "max_rel_err": max_rel,
            "baseline_ms": baseline,
            "candidate_ms": cand_ms,
        }
