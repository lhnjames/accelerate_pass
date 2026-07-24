#!/usr/bin/env python3
"""Run equal-budget random/GP search over a shared LLVM 21 parameter catalog."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from src.build_utils import compile_c, run_timing
from src.config import ConfigLoader
from src.correctness import check_correctness, detect_correctness_mode
from src.search_baselines import (
    Candidate, Evaluation, ParameterAxis, bayesian_search,
    build_candidate_catalog, random_search,
)
from src.toolchain_guard import verify_llvm21_toolchain


def _is_timing_only_define(flag: str) -> bool:
    """Return whether a PolyBench timing macro must not enter correctness builds.

    Correctness binaries must emit only the data used by the comparator.  Keeping
    this filter in the harness prevents a mistakenly supplied ``--define`` from
    making timing instrumentation part of the compared output.
    """
    return flag.strip() in {"-DPOLYBENCH_TIME", "-DPOLYBENCH_TIME=1"}


def _correctness_base_defines(defines: list[str]) -> list[str]:
    return [flag for flag in defines if not _is_timing_only_define(flag)]


def _atomic_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def _load_axes(path: Path) -> list[ParameterAxis]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, list):
        raise ValueError("axis file must contain a JSON list")
    return [ParameterAxis(str(item["flag"]), tuple(item["values"]))
            for item in raw]


def _compile(
    config, sources: list[str], include_dirs: list[str], defines: list[str],
    output: Path, extra_flags: list[str],
) -> tuple[bool, str]:
    return compile_c(
        config.compiler.clang_path, sources, include_dirs, defines,
        str(output), extra_flags=extra_flags,
        timeout=config.compiler.timeout_seconds,
        clang_cxx_path=config.compiler.clang_cxx_path)


def _evaluate_candidate(
    candidate: Candidate, *, config, sources, include_dirs, defines,
    correctness_base_defines, correctness_defines, baseline_ms: float,
    baseline_correctness_bin: Path,
    correctness_mode: str, work_dir: Path, pin_cpu: int | None,
    runs: int, epsilon: float, ordinal: int,
) -> Evaluation:
    started = time.monotonic()
    timing_bin = work_dir / f"candidate-{ordinal}.bin"
    ok, error = _compile(
        config, sources, include_dirs, defines, timing_bin,
        list(candidate.flags))
    if not ok:
        return Evaluation(-1.0e12, False, f"timing compile failed: {error[:500]}")

    correctness_bin = work_dir / f"candidate-{ordinal}-correctness.bin"
    ok, error = _compile(
        config, sources, include_dirs,
        correctness_base_defines + correctness_defines,
        correctness_bin, list(candidate.flags))
    if not ok:
        return Evaluation(-1.0e12, False,
                          f"correctness compile failed: {error[:500]}")
    passed, message = check_correctness(
        str(baseline_correctness_bin), str(correctness_bin), correctness_mode,
        epsilon=epsilon, timeout=config.runtime.measurement_timeout)
    if not passed:
        return Evaluation(-1.0e12, False, f"correctness failed: {message}")

    elapsed_ms = run_timing(str(timing_bin), runs=runs, pin_cpu=pin_cpu)
    if elapsed_ms <= 0:
        return Evaluation(-1.0e12, False, "candidate timing failed")
    return Evaluation(baseline_ms / elapsed_ms, True,
                      f"time_ms={elapsed_ms:.6f}; wall_s={time.monotonic()-started:.3f}")


def run_baselines(args: argparse.Namespace) -> dict:
    config = ConfigLoader(str(Path(args.config_dir).resolve())).load_all()
    identity = verify_llvm21_toolchain(config.compiler)
    axes = _load_axes(Path(args.axes))
    catalog = build_candidate_catalog(axes, max_candidates=args.max_catalog)
    source_list = [str(Path(args.source).resolve())]
    if args.polybench_source:
        source_list.append(str(Path(args.polybench_source).resolve()))
    include_dirs = [str(Path(item).resolve()) for item in args.include_dir]
    defines = list(args.define)
    correctness_base_defines = _correctness_base_defines(defines)
    correctness_defines = list(args.correctness_define)
    pin_cpu = args.pin_cpu

    result_dir = Path(args.result_dir).resolve()
    result_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=result_dir, prefix="eval-") as temporary:
        work_dir = Path(temporary)
        baseline_bin = work_dir / "baseline.bin"
        ok, error = _compile(config, source_list, include_dirs, defines,
                             baseline_bin, [])
        if not ok:
            raise RuntimeError(f"baseline compile failed: {error[:1000]}")
        baseline_ms = run_timing(str(baseline_bin), runs=args.runs,
                                 pin_cpu=pin_cpu)
        if baseline_ms <= 0:
            raise RuntimeError("baseline timing failed")

        baseline_correctness_bin = work_dir / "baseline-correctness.bin"
        ok, error = _compile(
            config, source_list, include_dirs,
            correctness_base_defines + correctness_defines,
            baseline_correctness_bin, [])
        if not ok:
            raise RuntimeError(f"baseline correctness compile failed: {error[:1000]}")
        correctness_mode = detect_correctness_mode(
            str(baseline_correctness_bin), timeout=config.runtime.measurement_timeout)

        axes_digest = hashlib.sha256(
            Path(args.axes).read_bytes()).hexdigest()
        output = {
            "schema_version": 1,
            "started_at_utc": datetime.now(timezone.utc).isoformat(),
            "program": str(Path(args.source).resolve()),
            "catalog_size": len(catalog),
            "catalog_sha256": hashlib.sha256(
                json.dumps([candidate.flags for candidate in catalog],
                           separators=(",", ":")).encode()).hexdigest(),
            "axes_sha256": axes_digest,
            "toolchain": identity.to_dict(),
            "baseline_ms": baseline_ms,
            "correctness_mode": correctness_mode,
            "budget": args.budget,
            "runs": args.runs,
            "pin_cpu": pin_cpu,
            "timing_defines": defines,
            "correctness_base_defines": correctness_base_defines,
            "seeds": args.seeds,
            "methods": {},
        }
        for method in ("random", "bayesian"):
            if args.method not in ("both", method):
                continue
            method_results = []
            for seed in args.seeds:
                ordinal = [0]

                def evaluate(candidate):
                    current = ordinal[0]
                    ordinal[0] += 1
                    return _evaluate_candidate(
                        candidate, config=config, sources=source_list,
                        include_dirs=include_dirs, defines=defines,
                        correctness_base_defines=correctness_base_defines,
                        correctness_defines=correctness_defines,
                        baseline_ms=baseline_ms,
                        baseline_correctness_bin=baseline_correctness_bin,
                        correctness_mode=correctness_mode, work_dir=work_dir,
                        pin_cpu=pin_cpu, runs=args.runs, epsilon=args.epsilon,
                        ordinal=current)

                if method == "random":
                    result = random_search(catalog, args.budget, seed, evaluate)
                else:
                    result = bayesian_search(
                        catalog, args.budget, seed, evaluate,
                        initial_points=args.initial_points)
                method_results.append(result.to_dict())
            output["methods"][method] = method_results
    output["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    return output


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--polybench-source", default=None)
    parser.add_argument("--axes", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--config-dir", default=str(PROJECT_ROOT / "configs"))
    parser.add_argument("--include-dir", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--correctness-define", action="append",
                        default=["-DSMALL_DATASET", "-DPOLYBENCH_DUMP_ARRAYS"])
    parser.add_argument("--method", choices=["random", "bayesian", "both"],
                        default="both")
    parser.add_argument("--seeds", type=int, nargs="+",
                        default=[101, 211, 307, 401, 503])
    parser.add_argument("--budget", type=int, default=20)
    parser.add_argument("--initial-points", type=int, default=5)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--pin-cpu", type=int, default=None)
    parser.add_argument("--epsilon", type=float, default=1e-4)
    parser.add_argument("--max-catalog", type=int, default=100_000)
    return parser


def main() -> None:
    args = make_parser().parse_args()
    result_path = Path(args.result_dir).resolve() / "search_baselines.json"
    try:
        result = run_baselines(args)
    except Exception as exc:
        result = {
            "schema_version": 1, "status": "failed",
            "error": f"{type(exc).__name__}: {exc}",
        }
        _atomic_json(result_path, result)
        raise SystemExit(1)
    _atomic_json(result_path, result)
    print(json.dumps({"status": "ok", "result": str(result_path)}))


if __name__ == "__main__":
    main()
