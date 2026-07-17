"""
Run Logger: structured logging for COMET optimization runs.

Every run is saved to:
  <project_root>/runs/{YYYY-MM-DD_HH-MM-SS}_{program_name}/
    full.log          — complete stdout (all prints, pass info, timing)
    llm_calls.jsonl   — each LLM call: role/content prompt + full response
    results.json      — final speedup, best flags, best source
    compile_cmds.log  — all compilation commands and outcomes

Usage:
    from src.run_logger import RunLogger, LoggingLLMClient
    logger = RunLogger(program_name="gemm", dataset="polybench")
    llm = LoggingLLMClient(inner_llm, logger)
    # stdout is automatically captured to full.log
"""

from __future__ import annotations
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

# src/run_logger.py -> src/ -> <project_root>/runs
RUNS_ROOT = Path(__file__).resolve().parent.parent / "runs"


class RunLogger:
    """Creates a run directory and provides structured log writers."""

    def __init__(self, program_name: str, dataset: str = "unknown",
                 extra_tag: str = ""):
        ts = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        tag = f"_{extra_tag}" if extra_tag else ""
        self.run_dir = RUNS_ROOT / f"{ts}_{dataset}_{program_name}{tag}"
        self.run_dir.mkdir(parents=True, exist_ok=True)

        self._full_log  = open(self.run_dir / "full.log",         "w", buffering=1)
        self._llm_log   = open(self.run_dir / "llm_calls.jsonl",  "w", buffering=1)
        self._cmd_log   = open(self.run_dir / "compile_cmds.log", "w", buffering=1)

        # Tee stdout → both console and full.log
        self._orig_stdout = sys.stdout
        sys.stdout = _Tee(self._orig_stdout, self._full_log)

        self._write_meta(program_name, dataset)
        print(f"[RunLogger] 运行日志目录: {self.run_dir}")

    def _write_meta(self, program_name: str, dataset: str):
        meta = {
            "program": program_name,
            "dataset": dataset,
            "start_time": datetime.now().isoformat(),
            "run_dir": str(self.run_dir),
        }
        (self.run_dir / "meta.json").write_text(json.dumps(meta, indent=2))

    def log_llm_call(self, call_type: str, messages: List[Dict],
                     response: Optional[str], elapsed_s: float,
                     step: int = -1, temperature: Optional[float] = None,
                     max_tokens: Optional[int] = None):
        """Record one LLM call with full prompt and response."""
        record = {
            "ts":          datetime.now().isoformat(),
            "step":        step,
            "call_type":   call_type,
            "temperature": temperature,
            "max_tokens":  max_tokens,
            "elapsed_s":   round(elapsed_s, 2),
            "messages":    messages,
            "response":    response,
        }
        self._llm_log.write(json.dumps(record, ensure_ascii=False) + "\n")
        self._llm_log.flush()

    def log_compile_cmd(self, cmd: List[str], ok: bool, stderr: str,
                        elapsed_s: float):
        """Record a compilation command and its outcome."""
        line = {
            "ts":       datetime.now().isoformat(),
            "cmd":      cmd,
            "ok":       ok,
            "stderr":   stderr[:500] if stderr else "",
            "elapsed_s": round(elapsed_s, 2),
        }
        self._cmd_log.write(json.dumps(line, ensure_ascii=False) + "\n")
        self._cmd_log.flush()

    def save_results(self, results: Dict[str, Any]):
        """Write final results.json."""
        results["end_time"] = datetime.now().isoformat()
        results["run_dir"]  = str(self.run_dir)
        (self.run_dir / "results.json").write_text(
            json.dumps(results, indent=2, ensure_ascii=False))

    def snapshot(self, name: str, content: str):
        """Save a named source snapshot to the run directory."""
        snapshots = self.run_dir / "snapshots"
        snapshots.mkdir(exist_ok=True)
        (snapshots / name).write_text(content)

    def close(self):
        sys.stdout = self._orig_stdout
        for f in (self._full_log, self._llm_log, self._cmd_log):
            try:
                f.close()
            except Exception:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class _Tee:
    """Write to two streams simultaneously."""
    def __init__(self, a, b):
        self.a, self.b = a, b

    def write(self, data):
        self.a.write(data)
        self.b.write(data)

    def flush(self):
        self.a.flush()
        self.b.flush()

    def fileno(self):
        return self.a.fileno()

    def isatty(self):
        return getattr(self.a, "isatty", lambda: False)()


class LoggingLLMClient:
    """
    Wrapper around LLMClient that logs every call to the RunLogger.
    Drop-in replacement — exposes the same .call() interface.
    """

    def __init__(self, inner, logger: RunLogger, step_getter=None):
        """
        inner:       the real LLMClient
        logger:      RunLogger instance
        step_getter: optional callable that returns the current step number
        """
        self._inner      = inner
        self._logger     = logger
        self._step_getter = step_getter
        self.call_count  = 0
        # Forward any attributes the code might access
        self.config      = inner.config

    def call(self, messages, temperature=None, max_tokens=None,
             timeout=None, call_type="agent") -> Optional[str]:
        step = self._step_getter() if self._step_getter else -1
        t0   = time.monotonic()
        resp = self._inner.call(messages, temperature=temperature,
                                max_tokens=max_tokens, timeout=timeout)
        elapsed = time.monotonic() - t0
        self.call_count += 1
        self._logger.log_llm_call(
            call_type=call_type,
            messages=messages,
            response=resp,
            elapsed_s=elapsed,
            step=step,
            temperature=temperature,
            max_tokens=max_tokens,
        )
        return resp

    def health_check(self):
        return self._inner.health_check()

    def parse_json_response(self, response):
        return self._inner.parse_json_response(response)
