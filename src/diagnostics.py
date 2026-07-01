"""Clean up clang's human-formatted diagnostics for LLM consumption.

clang's default text diagnostics interleave the actual error message with a
source-code snippet and a caret/tilde underline pointing at the offending
column (e.g. `^~~~~`). That layout is meant for a human eye scanning aligned
columns; it wastes tokens and gives an LLM nothing it can use, since the
column position is already stated numerically in the header line. This module
strips everything except the structured `file:line:col: severity: message`
header lines.
"""

import re

_DIAG_HEADER = re.compile(
    r'^(?P<file>[^\s:]+):(?P<line>\d+):(?P<col>\d+):\s+'
    r'(?P<severity>error|warning|note):\s+(?P<msg>.*)$'
)


def clean_clang_diagnostics(stderr: str, max_diagnostics: int = 8,
                             keep_severities=("error", "note")) -> str:
    """
    Compress clang's stderr into agent-friendly structured text:
    - drop source-snippet lines, caret(^)/tilde(~) underlines, and fix-it lines
    - keep one line per diagnostic: "file:line:col: severity: message"
    - truncate by diagnostic count (not by character count) so the last kept
      diagnostic is never cut off mid-sentence
    """
    if not stderr:
        return ""

    headers = []
    for line in stderr.splitlines():
        m = _DIAG_HEADER.match(line)
        if m:
            headers.append(m)

    if not headers:
        # No parseable diagnostics (linker error, crash, timeout, ...).
        # Fall back to returning the raw text as-is, capped by line count.
        lines = stderr.splitlines()
        return "\n".join(lines[:max_diagnostics * 3]).strip()

    kept = [m for m in headers if m.group("severity") in keep_severities]
    if not kept:
        # Filtering removed everything (e.g. only warnings) — don't make it
        # look like there were no diagnostics at all.
        kept = headers

    total = len(kept)
    kept = kept[:max_diagnostics]
    out_lines = [m.group(0) for m in kept]
    if total > len(kept):
        out_lines.append(f"... ({total - len(kept)} more diagnostics omitted)")
    return "\n".join(out_lines)
