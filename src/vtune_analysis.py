"""
vtune_analysis.py — VTune CLI wrapper for hardware-level profiling.

Collects Memory Bound%, top hotspot functions, and vectorization effectiveness
using Intel VTune's hardware-based hotspot collection. Designed to complement
perf stat with Top-Down microarchitecture analysis.

Usage:
    from src.vtune_analysis import is_vtune_available, collect_vtune_stats, format_vtune_summary
    if is_vtune_available():
        stats = collect_vtune_stats(binary_path, "/tmp/vtune_result_dir")
        print(format_vtune_summary(stats))
"""
import csv
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List, Optional


def is_vtune_available() -> bool:
    """
    Check whether vtune is installed and functional on this system.
    Requires both: binary exists AND exit code 0 from a safe probe.
    """
    if not shutil.which("vtune"):
        return False
    try:
        r = subprocess.run(
            ["vtune", "-help"],
            capture_output=True, text=True, timeout=15
        )
        return r.returncode == 0
    except Exception:
        return False


def collect_vtune_stats(binary_path: str,
                        result_dir: str,
                        timeout: int = 300) -> dict:
    """
    Run VTune hotspot collection on binary_path and parse the CSV report.

    Two-phase process:
      Phase 1: vtune -collect hotspots -knob sampling-mode=hw
               -finalization-mode full -result-dir <dir> -- <binary>
      Phase 2: vtune -report hotspots -result-dir <dir>
               -format csv -report-output <csv>

    Returns dict with keys (all optional, None when not available):
      vtune_available: bool
      memory_bound_pct: float | None   (% of CPU time that is memory-bound)
      top_hotspots: List[{func, cpu_time_pct, memory_bound_pct}]
      vectorized_time_pct: float | None
      error: str | None
    """
    result = {
        "vtune_available": True,
        "memory_bound_pct": None,
        "top_hotspots": [],
        "vectorized_time_pct": None,
        "error": None,
    }

    # VTune refuses to overwrite existing result directories
    rdir = Path(result_dir)
    if rdir.exists():
        shutil.rmtree(rdir, ignore_errors=True)
    rdir.mkdir(parents=True, exist_ok=True)

    csv_path = str(rdir / "hotspot_report.csv")

    # ── Phase 1: collect ─────────────────────────────────────────────────────
    # Try hardware hotspot collection first; fall back to runss (stack-sampling
    # SW collector that works without a hardware PMU / vtsspp kernel driver).
    # Return code 4 = partial success — VTune still generated valid data.
    def _run_collect(cmd):
        return subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=timeout, errors="replace"
        )

    try:
        hw_cmd = [
            "vtune",
            "-collect", "hotspots",
            "-knob", "sampling-mode=hw",
            "-result-dir", str(rdir),
            "-quiet",
            "--", binary_path,
        ]
        r1 = _run_collect(hw_cmd)

        if r1.returncode not in (0, 4):
            err_lower = (r1.stderr or "").lower()
            # Fall back to runss for any hardware PMU / processor incompatibility:
            # - "pmu-type" / "event-config": missing vtsspp kernel driver
            # - "not applicable": VTune cannot recognize the processor model
            # - "ptrace" / "yama": security policy blocks hw sampling
            if any(x in err_lower for x in ("pmu-type", "event-config",
                                             "not applicable", "cannot recognize",
                                             "ptrace", "yama")):
                shutil.rmtree(rdir, ignore_errors=True)
                rdir.mkdir(parents=True, exist_ok=True)
                runss_cmd = [
                    "vtune",
                    "-collect-with", "runss",
                    "-knob", "cpu-samples-mode=stack",
                    "-knob", "signals-mode=stack",
                    "-knob", "waits-mode=stack",
                    "-knob", "io-mode=stack",
                    "-call-stack-mode", "user-only",
                    "-result-dir", str(rdir),
                    "-quiet",
                    "--", binary_path,
                ]
                r1 = _run_collect(runss_cmd)
                if r1.returncode not in (0, 4):
                    result["error"] = (
                        f"vtune runss fallback failed (rc={r1.returncode}): "
                        f"{r1.stderr[:300]}"
                    )
                    return result
            else:
                result["error"] = (
                    f"vtune hotspots collect failed (rc={r1.returncode}): "
                    f"{r1.stderr[:300]}"
                )
                return result

    except subprocess.TimeoutExpired:
        result["error"] = f"vtune collect timed out after {timeout}s"
        return result
    except Exception as e:
        result["error"] = f"vtune collect exception: {e}"
        return result

    # ── Phase 2: report ──────────────────────────────────────────────────────
    # Use tab-delimited CSV with -group-by function and -show-as values for
    # stable, parseable output across VTune versions.
    report_cmd = [
        "vtune",
        "-report", "hotspots",
        "-result-dir", str(rdir),
        "-group-by", "function",
        "-format", "csv",
        "-csv-delimiter", "tab",
        "-show-as", "values",
        "-report-output", csv_path,
    ]
    try:
        r2 = subprocess.run(
            report_cmd,
            capture_output=True, text=True,
            timeout=60, errors="replace"
        )
        if r2.returncode not in (0, 4):
            result["error"] = f"vtune report failed (rc={r2.returncode}): {r2.stderr[:200]}"
            return result
    except Exception as e:
        result["error"] = f"vtune report exception: {e}"
        return result

    # ── Parse CSV ────────────────────────────────────────────────────────────
    if not Path(csv_path).exists():
        result["error"] = "vtune report CSV not generated"
        return result

    _parse_vtune_csv(csv_path, result)
    return result


def _parse_vtune_csv(csv_path: str, result: dict) -> None:
    """
    Parse VTune hotspot CSV into result dict (in-place).
    Column names vary by VTune version — parse defensively.
    Supports both comma-delimited and tab-delimited (-csv-delimiter tab) output.
    """
    try:
        with open(csv_path, newline="", encoding="utf-8", errors="replace") as f:
            content = f.read()

        # VTune CSV sometimes has metadata lines before the real header row.
        # Find the first line that looks like a header (contains "CPU Time" or "Function").
        lines = content.splitlines()
        header_idx = None
        for i, line in enumerate(lines):
            if re.search(r"cpu.?time|function", line, re.IGNORECASE):
                header_idx = i
                break

        if header_idx is None:
            result["error"] = "vtune CSV: no recognizable header row"
            return

        data_lines = "\n".join(lines[header_idx:])
        # Auto-detect delimiter: tab takes priority when the header contains a tab.
        delimiter = "\t" if "\t" in lines[header_idx] else ","
        reader = csv.DictReader(data_lines.splitlines(), delimiter=delimiter)
        rows = list(reader)
    except Exception as e:
        result["error"] = f"vtune CSV parse error: {e}"
        return

    if not rows:
        result["error"] = "vtune CSV: no data rows"
        return

    # Normalize column names (lowercase, strip spaces/quotes)
    def norm(s: str) -> str:
        return re.sub(r"[^a-z0-9]", "", s.lower())

    first_row = rows[0]
    col_map: Dict[str, str] = {}
    for raw_col in first_row.keys():
        col_map[norm(raw_col)] = raw_col

    def get_col(key_pat: str) -> Optional[str]:
        """Find column name matching a normalized pattern."""
        for normed, raw in col_map.items():
            if key_pat in normed:
                return raw
        return None

    col_func    = get_col("function")
    col_cpu     = get_col("cputime")
    col_membnd  = get_col("memorybound") or get_col("membound")
    col_vectime = get_col("vectorizedtime") or get_col("vectorized")

    def parse_float(val: Optional[str]) -> Optional[float]:
        if not val:
            return None
        val = val.strip().replace("%", "").replace(",", ".")
        try:
            return float(val)
        except ValueError:
            return None

    hotspots: List[dict] = []
    total_mem_bound_pct: Optional[float] = None

    for row in rows:
        func    = row.get(col_func, "").strip() if col_func else ""
        cpu_raw = parse_float(row.get(col_cpu)) if col_cpu else None
        mem_raw = parse_float(row.get(col_membnd)) if col_membnd else None
        vec_raw = parse_float(row.get(col_vectime)) if col_vectime else None

        if not func or cpu_raw is None:
            continue

        entry = {"func": func, "_cpu_raw": cpu_raw}
        if mem_raw is not None:
            entry["_mem_raw"] = mem_raw
        if vec_raw is not None:
            entry["_vec_raw"] = vec_raw
        hotspots.append(entry)

    if not hotspots:
        result["error"] = "vtune CSV: no valid function rows"
        return

    # Determine whether values are in % (>1 typical for percentages) or seconds
    # (-show-as values gives absolute seconds). Normalize to percentage by total.
    total_cpu_raw = sum(h["_cpu_raw"] for h in hotspots) or 1.0
    # If max raw value looks like already-percentage (e.g. 45.3 not 0.045), keep as-is.
    is_pct = any(h["_cpu_raw"] > 1.5 for h in hotspots)

    for h in hotspots:
        if is_pct:
            h["cpu_time_pct"] = round(h["_cpu_raw"], 1)
        else:
            h["cpu_time_pct"] = round(h["_cpu_raw"] / total_cpu_raw * 100.0, 1)
        if "_mem_raw" in h:
            h["memory_bound_pct"] = round(h["_mem_raw"], 1) if is_pct else None
        if "_vec_raw" in h:
            h["vectorized_time_pct"] = round(h["_vec_raw"], 1) if is_pct else None
        # Remove internal keys
        for k in ("_cpu_raw", "_mem_raw", "_vec_raw"):
            h.pop(k, None)

    hotspots.sort(key=lambda x: x["cpu_time_pct"], reverse=True)
    result["top_hotspots"] = hotspots[:10]

    # Aggregate memory bound: weighted average by CPU time contribution
    total_cpu_pct = sum(h["cpu_time_pct"] for h in hotspots) or 1.0
    mem_entries = [h for h in hotspots if h.get("memory_bound_pct") is not None]
    if mem_entries:
        weighted_mem = sum(
            h["memory_bound_pct"] * h["cpu_time_pct"]
            for h in mem_entries
        )
        result["memory_bound_pct"] = round(weighted_mem / total_cpu_pct, 1)

    vec_entries = [h for h in hotspots if h.get("vectorized_time_pct") is not None]
    if vec_entries:
        weighted_vec = sum(
            h["vectorized_time_pct"] * h["cpu_time_pct"]
            for h in vec_entries
        )
        result["vectorized_time_pct"] = round(weighted_vec / total_cpu_pct, 1)


def format_vtune_summary(stats: dict) -> str:
    """
    Produce LLM-friendly text from vtune stats dict.
    Designed to be included in the prompt as a supplementary profiling section.
    """
    if not stats.get("vtune_available"):
        return "  VTune: 不可用"
    if stats.get("error"):
        return f"  VTune 采集失败: {stats['error']}"

    lines = []

    mb = stats.get("memory_bound_pct")
    if mb is not None:
        quality = ("严重 (>50%)" if mb > 50
                   else "中等 (25-50%)" if mb > 25
                   else "轻度 (<25%)")
        lines.append(f"  Memory bound (VTune Top-Down): {mb:.1f}%  [{quality}]")

    vt = stats.get("vectorized_time_pct")
    if vt is not None:
        lines.append(f"  Vectorized time: {vt:.1f}%")

    hotspots = stats.get("top_hotspots", [])
    if hotspots:
        lines.append("  Top hotspot functions:")
        for h in hotspots[:3]:
            func  = h["func"][:40]
            cpu   = h["cpu_time_pct"]
            extra = ""
            if "memory_bound_pct" in h:
                extra = f"  mem_bound={h['memory_bound_pct']:.0f}%"
            lines.append(f"    {func}: {cpu:.1f}% CPU time{extra}")

    if not lines:
        lines.append("  VTune: 采集成功但无数值数据（可能为用户态采样模式）")

    return "\n".join(lines)
