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

# Tokenizes into "identifier" or "number" pieces, alternation tried in that
# order. A single-character lookaround can't reliably keep a bare \d+
# regex from matching digits embedded in an identifier printed as
# diagnostic text (e.g. TSVC's initialise_arrays() prints the loop's own
# name, "s1111": a lookbehind that only checks one character back still
# lets findall start a fresh number-match from the *second* digit of a
# digit run, since that character's own predecessor is a digit, not a
# letter). Matching the identifier as a whole token first makes finditer's
# scan position jump past all of "s1111" in one match, so it never gets a
# chance to start a number-match partway through it.
_TOKEN_RE = re.compile(
    r"[A-Za-z_][A-Za-z_0-9]*"
    r"|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
)

NumbersOrError = Union[list, str]


def _run_capture(bin_path: str, timeout: int = 60,
                 output_file: "Optional[Path]" = None):
    """Run a binary once, capture stdout+stderr as raw bytes (plus optional
    output-file bytes). Returns (returncode, combined_bytes, file_bytes) or
    None on timeout/launch failure.

    Deliberately NOT decoded with text=True/errors="replace": several
    CBench programs' real product output (compressed data, image/audio
    bytes) goes straight to stdout, and UTF-8-with-replacement decoding is
    lossy -- two genuinely different binary outputs can both contain
    invalid UTF-8 sequences that collapse to the same replacement
    character, making the hash tier below falsely see them as equal. Raw
    bytes preserve exactly what the program produced; extract_numbers()
    decodes losslessly (latin1, a 1:1 byte<->codepoint mapping) only when
    it needs a str to regex-scan.
    """
    try:
        r = subprocess.run([str(bin_path)], capture_output=True, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None
    combined = (r.stdout or b"") + (r.stderr or b"")
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
    for m in _TOKEN_RE.finditer(text):
        tok = m.group(0)
        if tok[0].isalpha() or tok[0] == "_":
            continue  # identifier token, not a number -- skip without splitting it
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
    _, out1, file1 = run1
    combined1 = out1.decode("latin1") + (file1.decode("latin1") if file1 is not None else "")
    nums = extract_numbers(combined1)
    if isinstance(nums, list) and len(nums) >= 1:
        return "numeric"

    run2 = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    if run2 is None or run2[0] != 0:
        return "exit_only"
    _, out2, file2 = run2
    if out1 == out2 and file1 == file2:
        return "hash"
    return "exit_only"


def check_correctness(ref_bin: str, opt_bin: str, mode: str,
                      epsilon: float = 1e-4, timeout: int = 60,
                      output_file: "Optional[Path]" = None) -> tuple:
    """Run both binaries and compare per `mode`. Returns (ok, error_msg)."""
    ref = _run_capture(ref_bin, timeout=timeout, output_file=output_file)
    if ref is None:
        return False, "reference run timed out"
    ref_rc, ref_out, ref_file = ref
    if ref_rc != 0:
        return False, f"reference exited non-zero ({ref_rc})"

    opt = _run_capture(opt_bin, timeout=timeout, output_file=output_file)
    if opt is None:
        return False, "optimized run timed out"
    opt_rc, opt_out, opt_file = opt
    if opt_rc != 0:
        return False, f"optimized version returned non-zero exit code {opt_rc}"

    if mode == "numeric":
        ref_text = ref_out.decode("latin1") + (ref_file.decode("latin1") if ref_file is not None else "")
        opt_text = opt_out.decode("latin1") + (opt_file.decode("latin1") if opt_file is not None else "")
        ref_nums = extract_numbers(ref_text)
        opt_nums = extract_numbers(opt_text)
        return compare_numeric(ref_nums, opt_nums, epsilon=epsilon)

    if mode == "hash":
        ref_h = _hash(ref_out + (ref_file or b""))
        opt_h = _hash(opt_out + (opt_file or b""))
        if ref_h != opt_h:
            return False, f"output hash mismatch (ref={ref_h[:12]}, opt={opt_h[:12]})"
        return True, ""

    # exit_only: both exit codes already checked above
    return True, ""
