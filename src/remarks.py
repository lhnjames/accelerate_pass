"""
Shared LLVM optimization remarks extraction utilities.
Used by both tune_param.py and tune_source.py.
"""
import os
import re
import subprocess
import tempfile
from collections import defaultdict
from pathlib import Path

# clang remark pass names → opt pass class names
REMARK_PASS_MAP = {
    "slp-vectorizer":  "SLPVectorizerPass",
    "loop-vectorize":  "LoopVectorizePass",
    "loop-unroll":     "LoopUnrollPass",
    "licm":            "LICMPass",
    "inline":          "InlinerPass",
    "gvn":             "GVN",
    "dse":             "DSEPass",
    "loop-unswitch":   "SimpleLoopUnswitchPass",
    "loop-load-elim":  "LoopLoadEliminationPass",
    "loop-distribute": "LoopDistributePass",
}


def make_noinline_src(src: str, kernel_name: str) -> str:
    """Write a temp copy of src with __attribute__((noinline)) on kernel_name.
    Returns path to temp file (caller must unlink)."""
    fd, tmp = tempfile.mkstemp(suffix=".c")
    os.close(fd)
    with open(src, errors="replace") as f:
        code = f.read()
    patched = code.replace(
        f"void {kernel_name}(",
        f"__attribute__((noinline)) void {kernel_name}("
    )
    if patched == code:
        patched = re.sub(
            r"(static\s+)?void\s+" + re.escape(kernel_name) + r"\s*\(",
            r"__attribute__((noinline)) void " + kernel_name + r"(",
            code, count=1
        )
    with open(tmp, "w") as f:
        f.write(patched)
    return tmp


def extract_rich_remarks_yaml(clang: str, src: str,
                               utils: "Path",
                               source_dir: "Path",
                               kernel_name: str,
                               timeout: int = 120) -> dict:
    """
    Extract LLVM optimization remarks using clang -O3 -fsave-optimization-record=yaml.
    Compiles only the kernel source (not polybench.c) with __attribute__((noinline))
    so kernel_name stays as a named function and remarks are attributed to it.

    Returns: {pass_name: [entry]} where each entry has:
      {file, line, column, function, msg, type,
       vf, ic, fail_reason, cost, source_snippet}
    """
    import yaml as _yaml

    fd_rem, rem_yaml = tempfile.mkstemp(suffix=".remarks.yaml")
    os.close(fd_rem)
    tmp_src = make_noinline_src(src, kernel_name)

    cmd = [
        clang, "-O3", "-std=c99", "-g", "-c",
        f"-I{utils}", f"-I{source_dir}",
        "-DLARGE_DATASET", "-DPOLYBENCH_TIME",
        "-fsave-optimization-record=yaml",
        f"-foptimization-record-file={rem_yaml}",
        tmp_src, "-o", "/dev/null",
    ]

    by_pass: dict = defaultdict(list)
    try:
        subprocess.run(cmd, capture_output=True, text=True,
                       timeout=timeout, errors="replace")

        if not Path(rem_yaml).exists() or Path(rem_yaml).stat().st_size == 0:
            return {}

        src_lines: list = []
        try:
            with open(src, errors="replace") as f:
                src_lines = f.readlines()
        except Exception:
            pass

        with open(rem_yaml, errors="replace") as f:
            content = f.read()

        class _TagLoader(_yaml.SafeLoader):
            pass
        def _tag_constructor(loader, tag_suffix, node):
            d = loader.construct_mapping(node, deep=True)
            d["!"] = tag_suffix
            return d
        _yaml.add_multi_constructor("!", _tag_constructor, Loader=_TagLoader)

        tmp_basename = os.path.basename(tmp_src)
        src_basename = os.path.basename(src)

        for doc in _yaml.load_all(content, Loader=_TagLoader):
            if not doc or not isinstance(doc, dict):
                continue

            remark_type = doc.get("!", "Unknown")
            raw_pass    = doc.get("Pass", "")
            pass_name   = REMARK_PASS_MAP.get(raw_pass, raw_pass)
            function    = doc.get("Function", "")
            debug_loc   = doc.get("DebugLoc", {}) or {}
            d_file      = debug_loc.get("File", "")
            d_line      = int(debug_loc.get("Line", 0))
            d_col       = int(debug_loc.get("Column", 0))
            args_raw    = doc.get("Args", []) or []

            vf, ic, fail_reason, cost_str, msg_parts = None, None, "", "", []
            for arg in args_raw:
                if not isinstance(arg, dict):
                    continue
                if "VectorizationFactor" in arg:
                    vf = int(arg["VectorizationFactor"])
                elif "InterleaveCount" in arg:
                    ic = int(arg["InterleaveCount"])
                elif "InstructionCost" in arg:
                    cost_str = str(arg["InstructionCost"])
                elif "String" in arg:
                    s = arg["String"]
                    msg_parts.append(s)
                    sl = s.lower()
                    if any(kw in sl for kw in
                           ["not vectorized", "cannot", "failed", "unsafe",
                            "aliasing", "dependency", "unknown", "invalid"]):
                        fail_reason += s
                else:
                    for k, v in arg.items():
                        if k not in ("!", "DebugLoc"):
                            msg_parts.append(f"{k}={v}")

            msg = "".join(msg_parts).strip()
            rtype_norm = ("missed"   if "Missed"   in remark_type else
                          "passed"   if "Passed"   in remark_type else
                          "analysis" if "Analysis" in remark_type else
                          "other")

            source_snippet = ""
            if src_lines and 1 <= d_line <= len(src_lines):
                s_start = max(0, d_line - 2)
                s_end   = min(len(src_lines), d_line + 1)
                snip    = []
                for i in range(s_start, s_end):
                    prefix = ">>> " if i == d_line - 1 else "    "
                    snip.append(f"{prefix}{i+1:4d}: {src_lines[i].rstrip()}")
                    if i == d_line - 1 and d_col > 0:
                        snip.append(" " * (8 + d_col) + "^")
                source_snippet = "\n".join(snip)

            if kernel_name and function and kernel_name not in function:
                continue

            display_file = os.path.basename(d_file) if d_file else ""
            if display_file == tmp_basename:
                display_file = src_basename

            entry = {
                "file":           display_file,
                "line":           d_line,
                "column":         d_col,
                "function":       function,
                "msg":            msg,
                "type":           rtype_norm,
                "vf":             vf,
                "ic":             ic,
                "fail_reason":    fail_reason.strip(),
                "cost":           cost_str,
                "source_snippet": source_snippet,
            }
            by_pass[pass_name].append(entry)

    except Exception:
        pass
    finally:
        try:
            os.unlink(rem_yaml)
        except Exception:
            pass
        try:
            os.unlink(tmp_src)
        except Exception:
            pass

    return dict(by_pass)


def format_rich_remarks_for_source_prompt(rich_remarks: dict,
                                           max_missed: int = 6) -> str:
    """
    Format rich remarks for the source-level LLM prompt.
    Shows source snippets, VF/IC, and failure reasons to guide code rewrites.
    Groups by pass, prioritizing vectorization misses.
    """
    PRIORITY = ["SLPVectorizerPass", "LoopVectorizePass", "LICMPass", "GVN",
                "LoopUnrollPass", "LoopLoadEliminationPass"]

    ordered = []
    for p in PRIORITY:
        if p in rich_remarks:
            ordered.append((p, rich_remarks[p]))
    for p, entries in rich_remarks.items():
        if p not in PRIORITY:
            ordered.append((p, entries))

    sections = []
    total_missed = 0
    for pass_name, entries in ordered:
        missed = [e for e in entries if e.get("type") == "missed"]
        passed = [e for e in entries if e.get("type") == "passed"]
        if not missed and not passed:
            continue

        sec = [f"[{pass_name}] {len(missed)} missed, {len(passed)} applied"]
        shown = 0
        for e in missed:
            if total_missed >= max_missed:
                break
            loc  = f"{e.get('file','?')}:{e.get('line',0)}"
            msg  = e.get("msg", "")
            vf   = e.get("vf")
            snip = e.get("source_snippet", "")
            sec.append(f"  MISSED @ {loc}: {msg[:120]}"
                       + (f"  [VF={vf}]" if vf else ""))
            if snip:
                for sl in snip.split("\n"):
                    sec.append(f"    {sl}")
            total_missed += 1
            shown += 1

        for e in passed[:1]:
            loc = f"{e.get('file','?')}:{e.get('line',0)}"
            vf  = e.get("vf")
            ic  = e.get("ic")
            msg = e.get("msg", "")
            sec.append(f"  OK    @ {loc}: {msg[:80]}"
                       + (f"  VF={vf}" if vf else "")
                       + (f", IC={ic}" if ic else ""))

        sections.append("\n".join(sec))

    return "\n\n".join(sections) if sections else "(no remarks)"
