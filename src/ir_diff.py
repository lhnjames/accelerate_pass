"""
ir_diff.py — Per-pass IR before/after comparison for LLVM O3 pipeline.

For each pass in the O3 pipeline, runs it standalone on the O0 IR and
compares the IR statistics before and after to determine whether the pass
actually modified the IR.

This answers the critical question: "Did this pass FIRE or was it a no-op?"
which is distinct from "Did this pass RUN?" (shown by -debug-pass-manager).

Usage:
    from src.ir_diff import check_passes_ir_changes, format_ir_diff_table
    diffs = check_passes_ir_changes(opt_path, ir_path, pass_list)
    print(format_ir_diff_table(diffs))
"""
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List, Optional

# ── Pass class name → new-PM -passes= flag ───────────────────────────────────
# LLVM 11 class names (from -debug-pass-manager) vs new PM registration names.
# This mapping is version-specific; missing entries → skipped=True.
_PM_NAME_TO_PASSES_FLAG: Dict[str, str] = {
    "SLPVectorizerPass":              "slp-vectorizer",
    "LoopVectorizePass":              "loop-vectorize",
    "LICMPass":                       "licm",
    "GVNPass":                        "gvn",
    "LoopUnrollPass":                 "loop-unroll",
    "InstCombinePass":                "instcombine",
    "DSEPass":                        "dse",
    "MemCpyOptPass":                  "memcpyopt",
    "EarlyCSEPass":                   "early-cse",
    "ReassociatePass":                "reassociate",
    "SCCPPass":                       "sccp",
    "ADCEPass":                       "adce",
    "BDCEPass":                       "bdce",
    "JumpThreadingPass":              "jump-threading",
    "CorrelatedValuePropagationPass": "correlated-propagation",
    "LoopRotatePass":                 "loop-rotate",
    "LoopSimplifyPass":               "loop-simplify",
    "IndVarSimplifyPass":             "indvars",
    "LoopIdiomRecognizePass":         "loop-idiom",
    "LoopDeletionPass":               "loop-deletion",
    "LoopLoadEliminationPass":        "loop-load-elim",
    "LoopDistributePass":             "loop-distribute",
    "LoopFlattenPass":                "loop-flatten",
    "LoopInterchangePass":            "loop-interchange",
    "SimpleLoopUnswitchPass":         "simple-loop-unswitch",
    "ConstraintEliminationPass":      "constraint-elimination",
    "SROA":                           "sroa",
    "AlwaysInlinerPass":              "always-inline",
    "InlinerPass":                    "inline",
}

# Passes that require module-level analysis and cannot run on a function IR file
# without wrapping — mark these as skipped rather than trying
_ANALYSIS_ONLY_PASSES = {
    "InlinerPass", "AlwaysInlinerPass",
    "GlobalOptPass", "DeadArgumentEliminationPass",
    "IPSCCPPass",
}


def _parse_ir_stats(ir_text: str) -> dict:
    """
    Count IR features directly from text — no subprocess call.
    Returns same schema as tune_param.get_ir_stats() for compatibility.
    """
    lines = ir_text.splitlines()

    vector_ops  = sum(1 for l in lines if re.search(r"<\d+ x ", l))
    fmul        = sum(1 for l in lines if "fmul" in l)
    fadd        = sum(1 for l in lines if "fadd" in l)
    phi_nodes   = sum(1 for l in lines if re.match(r"\s+%\S+\s*=\s*phi", l))
    # Count assignment instructions (rough total instruction count)
    total_instr = sum(1 for l in lines
                      if re.match(r"\s+(%\S+\s*=|store|call|ret|br)", l))
    load_ops    = sum(1 for l in lines if re.match(r"\s+%\S+\s*=\s*load", l))
    store_ops   = sum(1 for l in lines if re.match(r"\s+store\b", l))
    gep_ops     = sum(1 for l in lines
                      if re.match(r"\s+%\S+\s*=\s*getelementptr", l))

    return {
        "vector_ops":  vector_ops,
        "fmul":        fmul,
        "fadd":        fadd,
        "phi_nodes":   phi_nodes,
        "total_instr": total_instr,
        "load_ops":    load_ops,
        "store_ops":   store_ops,
        "gep_ops":     gep_ops,
    }


def check_passes_ir_changes(opt_path: str,
                             ir_path: str,
                             pass_list: List[str],
                             kernel_name: str = "",
                             timeout: int = 30) -> dict:
    """
    For each pass in pass_list (up to 20), run it standalone on the IR file
    and compare IR stats before/after.

    Returns:
      {pass_name: {
          changed:     bool,
          skipped:     bool,    # True when pass couldn't run standalone
          delta_instr: int,
          delta_vec:   int,
          delta_phi:   int,
          delta_load:  int,
          delta_store: int,
          pct_change:  float,   # pct change in total_instr (0.0 if no instr)
          passes_flag: str,     # the -passes= flag used
      }}
    """
    # Read baseline IR
    try:
        with open(ir_path, "r", errors="replace") as f:
            baseline_ir = f.read()
    except Exception as e:
        return {}

    baseline_stats = _parse_ir_stats(baseline_ir)
    results: dict = {}

    # Only process first 20 passes to stay within time budget
    for pass_name in pass_list[:20]:
        entry: dict = {
            "changed":     False,
            "skipped":     False,
            "delta_instr": 0,
            "delta_vec":   0,
            "delta_phi":   0,
            "delta_load":  0,
            "delta_store": 0,
            "pct_change":  0.0,
            "passes_flag": "",
        }

        # Analysis-only passes: skip immediately
        if pass_name in _ANALYSIS_ONLY_PASSES:
            entry["skipped"] = True
            entry["passes_flag"] = "(analysis-only)"
            results[pass_name] = entry
            continue

        passes_flag = _PM_NAME_TO_PASSES_FLAG.get(pass_name)
        if not passes_flag:
            entry["skipped"] = True
            entry["passes_flag"] = "(no mapping)"
            results[pass_name] = entry
            continue

        entry["passes_flag"] = passes_flag

        # Run: opt -passes=<flag> -S <ir_path>
        cmd = [opt_path, f"-passes={passes_flag}", "-S", ir_path]
        try:
            r = subprocess.run(
                cmd,
                capture_output=True, text=True,
                timeout=timeout, errors="replace"
            )
            if r.returncode != 0 or not r.stdout.strip():
                entry["skipped"] = True
                results[pass_name] = entry
                continue

            after_ir = r.stdout
        except subprocess.TimeoutExpired:
            entry["skipped"] = True
            results[pass_name] = entry
            continue
        except Exception:
            entry["skipped"] = True
            results[pass_name] = entry
            continue

        after_stats = _parse_ir_stats(after_ir)

        di = after_stats["total_instr"] - baseline_stats["total_instr"]
        dv = after_stats["vector_ops"]  - baseline_stats["vector_ops"]
        dp = after_stats["phi_nodes"]   - baseline_stats["phi_nodes"]
        dl = after_stats["load_ops"]    - baseline_stats["load_ops"]
        ds = after_stats["store_ops"]   - baseline_stats["store_ops"]

        changed = (di != 0 or dv != 0 or dp != 0 or dl != 0 or ds != 0)

        base_instr = baseline_stats["total_instr"]
        pct = (abs(di) / base_instr * 100.0) if base_instr > 0 else 0.0

        entry.update({
            "changed":     changed,
            "delta_instr": di,
            "delta_vec":   dv,
            "delta_phi":   dp,
            "delta_load":  dl,
            "delta_store": ds,
            "pct_change":  round(pct, 1),
        })
        results[pass_name] = entry

    return results


def format_ir_diff_table(diffs: dict) -> str:
    """
    Format per-pass IR change results as a human/LLM-readable table.

    Example output:
      Pass IR Change Analysis (per-pass standalone opt run):
      Pass              | Status  | ΔVec | ΔInstr | ΔPhi | ΔLoad | ΔStore | Δ%
      ──────────────────────────────────────────────────────────────────────────
      SLPVectorizerPass | FIRED   |  +8  |   -14  |   0  |   -2  |    0   | 7.2%
      LoopUnrollPass    | no-op   |   0  |     0  |   0  |   0   |    0   | 0.0%
      GVNPass           | skipped |   —  |    —   |   —  |   —   |   —    | —
    """
    if not diffs:
        return "  (IR diff analysis not available)"

    header = (
        "Per-pass IR modification check (FIRED=IR actually changed, "
        "no-op=pass ran but did nothing):\n"
        f"  {'Pass':<30} {'Status':<8} {'ΔVec':>5} {'ΔInstr':>7} "
        f"{'ΔPhi':>5} {'ΔLoad':>6} {'ΔStore':>7} {'Δ%':>6}\n"
        f"  {'─'*30} {'─'*8} {'─'*5} {'─'*7} {'─'*5} {'─'*6} {'─'*7} {'─'*6}"
    )
    rows = [header]

    for pass_name, d in diffs.items():
        if d["skipped"]:
            rows.append(
                f"  {pass_name:<30} {'skipped':<8} {'—':>5} {'—':>7} "
                f"{'—':>5} {'—':>6} {'—':>7} {'—':>6}"
            )
        elif d["changed"]:
            rows.append(
                f"  {pass_name:<30} {'FIRED':<8} "
                f"{d['delta_vec']:>+5} {d['delta_instr']:>+7} "
                f"{d['delta_phi']:>+5} {d['delta_load']:>+6} "
                f"{d['delta_store']:>+7} {d['pct_change']:>5.1f}%"
            )
        else:
            rows.append(
                f"  {pass_name:<30} {'no-op':<8} "
                f"{'0':>5} {'0':>7} {'0':>5} {'0':>6} {'0':>7} {'0.0%':>6}"
            )

    return "\n".join(rows)
