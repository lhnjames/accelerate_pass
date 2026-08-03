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
_NONFINITE_RE = re.compile(
    r"(?<![A-Za-z_0-9])[-+]?(nan|inf(?:inity)?)(?![A-Za-z_0-9])",
    re.IGNORECASE,
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
    nonfinite = _NONFINITE_RE.search(text)
    if nonfinite:
        token = nonfinite.group(0).lower()
        return "NaN in output" if "nan" in token else "Inf in output"
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
        # Mixed relative/absolute tolerance: near zero, epsilon acts as an
        # absolute tolerance; at larger magnitudes it scales relatively.
        # Include both operands so comparison remains symmetric.
        denom = max(abs(a), abs(b), 1.0)
        err = abs(a - b) / denom
        if err > max_err:
            max_err, bad_idx = err, i
    if max_err > epsilon:
        return False, (f"Numeric mismatch: max relative error {max_err:.2e} at index {bad_idx} "
                       f"(ref={v1[bad_idx]!r}, opt={v2[bad_idx]!r}), epsilon={epsilon:.2e}")
    return True, ""


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _decode_text_output(data: bytes) -> "Optional[str]":
    """Return decoded text only when ``data`` is genuinely textual.

    Codec/compression benchmarks often emit arbitrary bytes that happen to
    contain ASCII digits.  Treating a latin1 view of those bytes as numeric
    output made bzip2 select the tolerant numeric checker instead of exact
    hashing.  UTF-8 validity alone is not sufficient (NUL/control-heavy data
    can still be valid), so require a high printable/whitespace ratio too.
    """
    if not data:
        return ""
    if b"\x00" in data:
        return None
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    printable = sum(ch.isprintable() or ch in "\r\n\t" for ch in text)
    if printable / max(1, len(text)) < 0.95:
        return None
    return text


def reference_health(bin_path: str, output_file: "Optional[Path]" = None,
                     timeout: int = 60) -> dict:
    """Is this reference binary actually doing its job?

    A benchmark that cannot find its input file is the worst kind of failure
    here, because nothing downstream notices: cBench's bzip2_encode prints
    "Can't open input file ..." to stderr and then EXITS 0, so the exit-code
    check passes, stdout is empty so the hash check compares nothing against
    nothing and passes too, and the program finishes in ~1 ms instead of ~85 ms
    -- at which point the harness measures noise on a no-op and reports it as a
    benchmark result. It happened on one of two nodes for a single missing
    file, and produced a plausible-looking 1.01x.

    Returns {ok, reason, stdout_bytes, stderr_bytes, ms} so callers can refuse
    to build a task on a broken reference instead of silently scoring it.
    """
    import time as _time
    t0 = _time.perf_counter()
    run = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    ms = (_time.perf_counter() - t0) * 1000.0
    if run is None:
        return {"ok": False, "reason": "reference run timed out or failed to launch",
                "stdout_bytes": 0, "stderr_bytes": 0, "ms": ms}
    rc, combined, file_bytes = run
    payload = combined + (file_bytes or b"")
    if rc != 0:
        return {"ok": False, "reason": f"reference exited {rc}",
                "stdout_bytes": len(payload), "stderr_bytes": 0, "ms": ms}
    if not payload.strip():
        return {"ok": False,
                "reason": "reference produced no output -- benchmark likely not "
                          "running (missing input file? wrong cwd?)",
                "stdout_bytes": 0, "stderr_bytes": 0, "ms": ms}
    lowered = payload.lower()
    for marker in (b"can't open", b"cannot open", b"no such file",
                   b"not found", b"permission denied"):
        if marker in lowered:
            return {"ok": False,
                    "reason": f"reference output contains an I/O error ({marker.decode()})",
                    "stdout_bytes": len(payload), "stderr_bytes": 0, "ms": ms}
    return {"ok": True, "reason": "", "stdout_bytes": len(payload),
            "stderr_bytes": 0, "ms": ms}


def detect_correctness_mode(bin_path: str, output_file: "Optional[Path]" = None,
                            timeout: int = 20) -> str:
    """Auto-detect which tier applies to an already-built reference binary.
    Always runs the binary twice: determinism of the *reference* is what
    decides the tier, and it also catches references that can't be validated
    at all. No special build flags or macros required -- this is just the
    binary that gets built for timing anyway.

    The tiers are NOT ordered "numeric is always richest". `numeric` accepts
    any output within a relative tolerance, which is the right gate for
    floating-point kernels (vectorization legitimately reassociates FP ops
    and perturbs the last bits) but is strictly WEAKER than `hash` for
    discrete output. Picking `numeric` merely because digits are present
    silently downgrades the gate on every checksum/codec/sort benchmark:
    telecom_crc32 prints a ~4e9 CRC value, and a 1e-4 *relative* tolerance
    on that lets a wrong checksum differ by ±400000 and still pass. Same
    hole for bzip2_decode's decompressed bytes, automotive_qsort1's sorted
    keys, network_dijkstra's path costs, and office_stringsearch2's offsets.

    So the deciding question is "can this output legitimately move at all?",
    answered by whether any reference value is non-integral:

      deterministic + some fractional value -> numeric  (FP data, tolerate)
      deterministic + all values integral   -> hash     (discrete, exact)
      deterministic + no numbers            -> hash     (text/binary, exact)
      non-deterministic                     -> see below

    Integral-valued output routed to `hash` may be stricter than strictly
    necessary (a kernel printing floats with "%.0f" loses its tolerance),
    but over-strictness only ever rejects a valid candidate -- it can never
    accept a wrong one, which is the correct failure direction for a
    correctness gate.
    """
    run1 = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    if run1 is None or run1[0] != 0:
        return "exit_only"
    run2 = _run_capture(bin_path, timeout=timeout, output_file=output_file)
    if run2 is None or run2[0] != 0:
        return "exit_only"
    _, out1, file1 = run1
    _, out2, file2 = run2
    text1 = _decode_text_output(out1 + (file1 or b""))
    nums1 = extract_numbers(text1) if text1 is not None else None

    if out1 == out2 and file1 == file2:
        if isinstance(nums1, list) and any(
                math.isfinite(v) and v != int(v) for v in nums1):
            return "numeric"
        return "hash"

    # Non-deterministic reference. `hash` is impossible, and `numeric` is
    # only meaningful if the NUMBERS are stable even though the surrounding
    # bytes are not (e.g. a timing line printed alongside the results). If
    # the values themselves move between two runs of the same binary, no
    # candidate can ever be validated against it -- every comparison result
    # is noise, so report the weakest tier rather than manufacturing verdicts.
    # This is not hypothetical: cBench security_sha's SHA_INFO.data overruns
    # its 64-byte block on LP64 and feeds uninitialised stack into the
    # digest, so its "reference" digest differs on every single run.
    text2 = _decode_text_output(out2 + (file2 or b""))
    nums2 = extract_numbers(text2) if text2 is not None else None
    if isinstance(nums1, list) and isinstance(nums2, list) and nums1:
        stable, _ = compare_numeric(nums1, nums2)
        if stable:
            return "numeric"
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
        ref_payload = ref_out + (ref_file or b"")
        # An empty reference makes the hash comparison vacuous -- it compares
        # the hash of nothing against the hash of nothing and always passes.
        # That is not hypothetical: cBench bzip2_encode could not open its
        # input file on one node, printed the error to stderr, and STILL EXITED
        # 0, so the reference produced no output, every candidate "matched" it,
        # and a 0.96 ms no-op was scored as a real 85 ms benchmark.
        if not ref_payload.strip():
            return False, ("reference produced no output at all -- the benchmark "
                           "is not running (missing input file? wrong working "
                           "directory?), so any comparison against it is vacuous")
        opt_payload = opt_out + (opt_file or b"")
        ref_h, opt_h = _hash(ref_payload), _hash(opt_payload)
        if ref_h != opt_h:
            return False, f"output hash mismatch (ref={ref_h[:12]}, opt={opt_h[:12]})"
        return True, ""

    # exit_only: both exit codes already checked above
    return True, ""
