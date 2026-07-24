"""Manifest-aware execution, correctness, and paired timing for real programs."""
from __future__ import annotations

import hashlib
import os
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from src.build_manifest import RuntimeContract
from src.correctness import _decode_text_output, compare_numeric, extract_numbers
from src.experiment_stats import PairedPerformanceSummary, summarize_paired_performance


@dataclass(frozen=True)
class RuntimeObservation:
    returncode: int
    elapsed_seconds: float
    stdout: bytes
    stderr: bytes
    output_files: tuple[tuple[str, bytes], ...]
    timed_out: bool = False
    error: str = ""

    @property
    def correctness_payload(self) -> bytes:
        # Declared products are authoritative. Console progress and diagnostic
        # timing text are deliberately excluded when product files exist.
        if self.output_files:
            chunks = []
            for _name, data in self.output_files:
                chunks.extend((len(data).to_bytes(8, "big"), data))
            return b"".join(chunks)
        return self.stdout + self.stderr

    @property
    def numeric_payload(self) -> bytes:
        if self.output_files:
            return b"\n".join(data for _name, data in self.output_files)
        return self.stdout + self.stderr


@dataclass(frozen=True)
class RuntimeCorrectnessResult:
    ok: bool
    mode: str
    message: str = ""


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


class RuntimeExecutor:
    """Execute one binary under a manifest RuntimeContract.

    Runtime ``argv`` includes a descriptive logical argv[0]. The actual
    executable path replaces that first entry; the remaining entries are
    passed unchanged. This keeps manifests relocatable across local and
    remote build directories.
    """

    def __init__(self, binary: "str | Path", contract: RuntimeContract,
                 *, timeout_seconds: int = 600, pin_cpu: Optional[int] = None):
        self.binary = Path(binary).resolve()
        self.contract = contract
        self.timeout_seconds = timeout_seconds
        self.pin_cpu = pin_cpu

    def _validate(self) -> None:
        if not self.binary.is_file():
            raise ValueError(f"runtime binary not found: {self.binary}")
        if not self.contract.argv:
            raise ValueError("runtime argv must include a logical argv[0]")
        if self.contract.stdin is not None and not self.contract.stdin.is_file():
            raise ValueError(f"runtime stdin not found: {self.contract.stdin}")
        for output in self.contract.output_files:
            if not _is_within(output, self.contract.cwd):
                raise ValueError(
                    f"refusing output outside runtime cwd: {output} not under "
                    f"{self.contract.cwd}")

    def run(self, *, capture_output: bool = True,
            clean_outputs: bool = True) -> RuntimeObservation:
        self._validate()
        self.contract.cwd.mkdir(parents=True, exist_ok=True)
        if clean_outputs:
            for output in self.contract.output_files:
                if output.is_file():
                    output.unlink()

        command = [str(self.binary), *self.contract.argv[1:]]
        if self.pin_cpu is not None:
            command = ["taskset", "-c", str(self.pin_cpu), *command]

        stdin_handle = None
        stdout_target = subprocess.PIPE if capture_output else subprocess.DEVNULL
        stderr_target = subprocess.PIPE if capture_output else subprocess.DEVNULL
        started = time.monotonic()
        try:
            if self.contract.stdin is not None:
                stdin_handle = self.contract.stdin.open("rb")
            proc = subprocess.Popen(
                command, cwd=str(self.contract.cwd), stdin=stdin_handle,
                stdout=stdout_target, stderr=stderr_target,
                start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=self.timeout_seconds)
                timed_out = False
                error = ""
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
                stdout, stderr = proc.communicate()
                timed_out = True
                error = f"runtime timed out after {self.timeout_seconds}s"
            elapsed = time.monotonic() - started
        except OSError as exc:
            elapsed = time.monotonic() - started
            return RuntimeObservation(-1, elapsed, b"", b"", (), error=str(exc))
        finally:
            if stdin_handle is not None:
                stdin_handle.close()

        products = []
        for output in self.contract.output_files:
            try:
                products.append((str(output), output.read_bytes()))
            except OSError:
                products.append((str(output), b""))
        return RuntimeObservation(
            proc.returncode, elapsed, stdout or b"", stderr or b"",
            tuple(products), timed_out=timed_out, error=error)


def detect_runtime_correctness_mode(
    executor: RuntimeExecutor,
) -> tuple[str, RuntimeObservation]:
    first = executor.run(capture_output=True)
    if first.timed_out or first.returncode != 0:
        return "exit_only", first
    numeric_payload = first.numeric_payload
    text = _decode_text_output(numeric_payload)
    if text is not None:
        numbers = extract_numbers(text)
        if isinstance(numbers, list) and numbers:
            return "numeric", first
    second = executor.run(capture_output=True)
    if (not second.timed_out and second.returncode == 0
            and second.correctness_payload == first.correctness_payload):
        return "hash", first
    return "exit_only", first


def compare_runtime_correctness(
    reference: RuntimeObservation,
    candidate: RuntimeObservation,
    mode: str,
    *,
    epsilon: float = 1e-4,
) -> RuntimeCorrectnessResult:
    for label, observation in (("reference", reference), ("candidate", candidate)):
        if observation.timed_out:
            return RuntimeCorrectnessResult(False, mode, f"{label} timed out")
        if observation.returncode != 0:
            return RuntimeCorrectnessResult(
                False, mode, f"{label} exited {observation.returncode}")

    if mode == "numeric":
        reference_payload = reference.numeric_payload
        candidate_payload = candidate.numeric_payload
        reference_text = _decode_text_output(reference_payload)
        candidate_text = _decode_text_output(candidate_payload)
        if reference_text is None or candidate_text is None:
            return RuntimeCorrectnessResult(False, mode, "numeric output is not text")
        ok, message = compare_numeric(
            extract_numbers(reference_text), extract_numbers(candidate_text),
            epsilon=epsilon)
        return RuntimeCorrectnessResult(ok, mode, message)
    if mode == "hash":
        reference_payload = reference.correctness_payload
        candidate_payload = candidate.correctness_payload
        ref_hash = hashlib.sha256(reference_payload).hexdigest()
        candidate_hash = hashlib.sha256(candidate_payload).hexdigest()
        if ref_hash != candidate_hash:
            return RuntimeCorrectnessResult(
                False, mode,
                f"output hash mismatch (ref={ref_hash[:12]}, "
                f"candidate={candidate_hash[:12]})")
        return RuntimeCorrectnessResult(True, mode)
    if mode == "exit_only":
        return RuntimeCorrectnessResult(True, mode)
    raise ValueError(f"unsupported correctness mode: {mode!r}")


def verify_runtime_pair(reference: RuntimeExecutor, candidate: RuntimeExecutor,
                        *, epsilon: float = 1e-4) -> RuntimeCorrectnessResult:
    mode, reference_observation = detect_runtime_correctness_mode(reference)
    candidate_observation = candidate.run(capture_output=True)
    return compare_runtime_correctness(
        reference_observation, candidate_observation, mode, epsilon=epsilon)


def measure_paired_runtime(
    reference: RuntimeExecutor,
    candidate: RuntimeExecutor,
    *,
    pairs: int = 9,
    bootstrap_seed: int = 0,
    bootstrap_resamples: int = 10_000,
) -> PairedPerformanceSummary:
    """Warm each binary, then collect balanced alternating A/B pairs."""
    if pairs <= 0:
        raise ValueError("pairs must be positive")
    for executor in (reference, candidate):
        warmup = executor.run(capture_output=False)
        if warmup.timed_out or warmup.returncode != 0:
            raise RuntimeError(f"warmup failed for {executor.binary}: {warmup.error}")

    baseline_samples = []
    candidate_samples = []
    for index in range(pairs):
        order = ((reference, baseline_samples), (candidate, candidate_samples))
        if index % 2:
            order = tuple(reversed(order))
        for executor, destination in order:
            observation = executor.run(capture_output=False)
            if observation.timed_out or observation.returncode != 0:
                raise RuntimeError(
                    f"timing run failed for {executor.binary}: "
                    f"{observation.error or observation.returncode}")
            destination.append(observation.elapsed_seconds)
    return summarize_paired_performance(
        baseline_samples, candidate_samples,
        bootstrap_seed=bootstrap_seed,
        bootstrap_resamples=bootstrap_resamples)
