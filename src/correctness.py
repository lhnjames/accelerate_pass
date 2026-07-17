"""
Generic, framework-independent correctness checking.

Replaces the old PolyBench-only trio (polybench_dump / stdout_compare /
exit_only), which required target programs to #include <polybench.h> and
call its POLYBENCH_DUMP_* macros in a specific begin/end-marker format.
This module works against ANY compiled program's natural stdout/stderr
(plus an optional output file) -- no source changes, no required header,
no required macro calls. See docs/GENERIC_HARNESS_DESIGN.md.

Three tiers, richest first:
  "numeric"   -- every numeric token in the captured output is compared
                 element-wise with a relative-error tolerance. No markers
                 required: this scans the whole text, so it's a strict
                 superset of the old marker-scoped parser (which broke on
                 single-line dumps where the value trails the marker with
                 no separator -- see docs/SPEC_mcf_r_build_status.md).
  "hash"      -- output is deterministic but not usefully numeric (e.g.
                 text/binary data): exact SHA256 match required.
  "exit_only" -- weakest: only that the process exits 0.
"""
from __future__ import annotations

import hashlib
import math
import re
import subprocess
from pathlib import Path
from typing import Optional, Union

FLOAT_RE = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")

NumbersOrError = Union[list, str]


def _run_capture(bin_path: str, timeout: int = 60,
                 output_file: "Optional[Path]" = None):
    """Run a binary once, capture stdout+stderr text and optional output-file
    bytes. Returns (returncode, combined_text, file_bytes) or None on
    timeout/launch failure."""
    try:
        r = subprocess.run([str(bin_path)], capture_output=True, text=True,
                           timeout=timeout, errors="replace")
    except (subprocess.TimeoutExpired, OSError):
        return None
    combined = (r.stdout or "") + (r.stderr or "")
    file_bytes = None
    if output_file is not None and output_file.exists():
        try:
            file_bytes = output_file.read_bytes()
        except OSError:
            file_bytes = None
    return r.returncode, combined, file_bytes


def extract_numbers(text: str) -> NumbersOrError:
    """Every numeric token in `text`, in order. No begin/end markers
    required -- unlike the old PolyBench-dump parser, this doesn't look
    for "begin dump:"/"end dump:" lines, it just scans the entire text.
    NaN/Inf always indicate a broken computation, never an accepted
    difference, so they short-circuit as errors rather than being silently
    dropped or compared."""
    values = []
    for tok in FLOAT_RE.findall(text):
        try:
            v = float(tok)
        except ValueError:
            continue
        if math.isnan(v):
            return "NaN in output"
        if math.isinf(v):
            return "Inf in output"
        values.append(v)
    return values


def compare_numeric(v1: NumbersOrError, v2: NumbersOrError,
                    epsilon: float = 1e-4) -> tuple:
    """Element-wise relative-error comparison. Returns (ok, message)."""
    if isinstance(v1, str):
        return False, f"Reference output error: {v1}"
    if isinstance(v2, str):
        return False, f"Optimized output error: {v2}"
    if len(v1) == 0:
        return False, "Reference output is empty (binary may have crashed or produced no output)"
    if len(v2) == 0:
        return False, "Optimized output is empty (binary may have crashed or produced no output)"
    if len(v1) != len(v2):
        return False, (f"Size mismatch: reference={len(v1)}, optimized={len(v2)}. "
                       f"Candidate likely changed the output structure.")
    if len(v1) > 4 and all(x == 0 for x in v1):
        return False, "Reference output is all zeros (suspicious -- check the reference build)"

    max_err, bad_idx = 0.0, -1
    for i, (a, b) in enumerate(zip(v1, v2)):
        denom = max(abs(a), 1e-8)
        err = abs(a - b) / denom
        if err > max_err:
            max_err, bad_idx = err, i
    if max_err > epsilon:
        return False, (f"Numeric mismatch: max relative error {max_err:.2e} at index {bad_idx} "
                       f"(ref={v1[bad_idx]!r}, opt={v2[bad_idx]!r}), epsilon={epsilon:.2e}")
    return True, ""


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def detect_correctness_mode(bin_path: str, output_file: "Optional[Path]" = None,
                            timeout: int = 20) -> str:
    """Auto-detect which tier applies to an already-built reference binary.
    Runs it once (twice if the first run has no numeric content, to check
    determinism for the hash tier). No special build flags or macros
    required -- this is just the binary that gets built for timing anyway."""
    run1 = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    if run1 is None or run1[0] != 0:
        return "exit_only"
    _, text1, file1 = run1
    file1_text = file1.decode("latin1") if file1 is not None else ""
    nums = extract_numbers(text1 + file1_text)
    if isinstance(nums, list) and len(nums) >= 1:
        return "numeric"

    run2 = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    if run2 is None or run2[0] != 0:
        return "exit_only"
    _, text2, file2 = run2
    if text1 == text2 and file1 == file2:
        return "hash"
    return "exit_only"


def check_correctness(ref_bin: str, opt_bin: str, mode: str,
                      epsilon: float = 1e-4, timeout: int = 60,
                      output_file: "Optional[Path]" = None) -> tuple:
    """Run both binaries and compare per `mode`. Returns (ok, error_msg)."""
    ref = _run_capture(ref_bin, timeout=timeout, output_file=output_file)
    if ref is None:
        return False, "reference run timed out"
    ref_rc, ref_text, ref_file = ref
    if ref_rc != 0:
        return False, f"reference exited non-zero ({ref_rc})"

    opt = _run_capture(opt_bin, timeout=timeout, output_file=output_file)
    if opt is None:
        return False, "optimized run timed out"
    opt_rc, opt_text, opt_file = opt
    if opt_rc != 0:
        return False, f"optimized version returned non-zero exit code {opt_rc}"

    if mode == "numeric":
        ref_file_text = ref_file.decode("latin1") if ref_file is not None else ""
        opt_file_text = opt_file.decode("latin1") if opt_file is not None else ""
        ref_nums = extract_numbers(ref_text + ref_file_text)
        opt_nums = extract_numbers(opt_text + opt_file_text)
        return compare_numeric(ref_nums, opt_nums, epsilon=epsilon)

    if mode == "hash":
        ref_h = _hash(ref_text.encode("utf-8", "replace") + (ref_file or b""))
        opt_h = _hash(opt_text.encode("utf-8", "replace") + (opt_file or b""))
        if ref_h != opt_h:
            return False, f"output hash mismatch (ref={ref_h[:12]}, opt={opt_h[:12]})"
        return True, ""

    # exit_only: both exit codes already checked above
    return True, ""
