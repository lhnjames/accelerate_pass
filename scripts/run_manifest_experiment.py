#!/usr/bin/env python3
"""Build, verify, and optionally time a multi-TU manifest with LLVM 21."""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from src.build_manifest import MultiTUBuilder, load_build_manifest
from src.config import ConfigLoader
from src.runtime_runner import (
    RuntimeExecutor, measure_paired_runtime, verify_runtime_pair)
from src.toolchain_guard import verify_llvm21_toolchain


def _atomic_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def _commands(commands) -> list[list[str]]:
    return [[str(part) for part in command] for command in commands]


def run_experiment(args: argparse.Namespace) -> tuple[int, dict]:
    config = ConfigLoader(str(Path(args.config_dir).resolve())).load_all()
    toolchain = verify_llvm21_toolchain(config.compiler)
    manifest = load_build_manifest(args.manifest)
    runtime = manifest.runtime_for(args.workload)

    result_dir = Path(args.result_dir).resolve()
    baseline_binary = result_dir / "bin" / f"{manifest.name}-o3"
    candidate_binary = result_dir / "bin" / f"{manifest.name}-candidate"
    builder = MultiTUBuilder(config.compiler)
    started = datetime.now(timezone.utc).isoformat()
    record = {
        "schema_version": 1,
        "benchmark": manifest.name,
        "manifest": str(manifest.manifest_path),
        "workload": args.workload or manifest.default_workload or "runtime",
        "started_at_utc": started,
        "toolchain": toolchain.to_dict(),
        "candidate_flags": list(args.candidate_flag),
        "pin_cpu": args.pin_cpu,
        "pairs": args.pairs,
    }

    baseline = builder.build(
        manifest, baseline_binary, result_dir / "objects" / "o3")
    record["baseline_build"] = {
        "success": baseline.success, "error": baseline.error,
        "commands": _commands(baseline.commands),
    }
    if not baseline.success:
        record["status"] = "baseline_build_failed"
        return 2, record

    candidate = builder.build(
        manifest, candidate_binary, result_dir / "objects" / "candidate",
        extra_flags=args.candidate_flag)
    record["candidate_build"] = {
        "success": candidate.success, "error": candidate.error,
        "commands": _commands(candidate.commands),
    }
    if not candidate.success:
        record["status"] = "candidate_build_failed"
        return 3, record

    reference_executor = RuntimeExecutor(
        baseline.binary, runtime, timeout_seconds=args.timeout,
        pin_cpu=args.pin_cpu)
    candidate_executor = RuntimeExecutor(
        candidate.binary, runtime, timeout_seconds=args.timeout,
        pin_cpu=args.pin_cpu)
    correctness = verify_runtime_pair(
        reference_executor, candidate_executor, epsilon=args.epsilon)
    record["correctness"] = {
        "ok": correctness.ok,
        "mode": correctness.mode,
        "message": correctness.message,
        "epsilon": args.epsilon,
    }
    if not correctness.ok:
        record["status"] = "correctness_failed"
        return 4, record

    if args.pairs:
        summary = measure_paired_runtime(
            reference_executor, candidate_executor, pairs=args.pairs,
            bootstrap_seed=args.bootstrap_seed,
            bootstrap_resamples=args.bootstrap_resamples)
        record["performance"] = summary.to_dict()
    record["status"] = "ok"
    record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    return 0, record


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", help="path to build_manifest.json")
    parser.add_argument("--config-dir", default=str(PROJECT_ROOT / "configs"))
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--workload", default=None,
                        help="named manifest workload, e.g. test or ref")
    parser.add_argument(
        "--candidate-flag", action="append", default=[],
        help="compiler flag; repeat it (use --candidate-flag=--name=value)")
    parser.add_argument("--pairs", type=int, default=0,
                        help="paired timing samples after correctness (default: 0)")
    parser.add_argument("--pin-cpu", type=int, default=None)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--epsilon", type=float, default=1e-4)
    parser.add_argument("--bootstrap-seed", type=int, default=0)
    parser.add_argument("--bootstrap-resamples", type=int, default=10_000)
    return parser


def main() -> None:
    args = make_parser().parse_args()
    result_path = Path(args.result_dir).resolve() / "experiment.json"
    try:
        code, record = run_experiment(args)
    except Exception as exc:
        record = {
            "schema_version": 1,
            "status": "setup_failed",
            "error": f"{type(exc).__name__}: {exc}",
            "finished_at_utc": datetime.now(timezone.utc).isoformat(),
        }
        code = 1
    record.setdefault("finished_at_utc", datetime.now(timezone.utc).isoformat())
    _atomic_json(result_path, record)
    print(json.dumps({
        "status": record["status"], "result": str(result_path)},
        sort_keys=True))
    raise SystemExit(code)


if __name__ == "__main__":
    main()
