"""
Pass pipeline extraction and visualization.

Runs LLVM opt with -debug-pass-manager to capture which passes executed
on the kernel function, then renders a graphviz DOT graph and returns a
structured summary for the LLM prompt.
"""
import re
import subprocess
from collections import defaultdict, OrderedDict
from pathlib import Path
from typing import Optional

# Visual colors per optimization category (hex for DOT)
_CAT_COLOR = {
    "vectorize": "#AED6F1",   # light blue
    "loop":      "#A9DFBF",   # light green
    "memory":    "#F9E79F",   # light yellow
    "scalar":    "#F5CBA7",   # light orange
    "inline":    "#D2B4DE",   # light purple
    "other":     "#D5D8DC",   # light grey
}

# Pass name → (short label, category)
_PASS_META = {
    "SLPVectorizerPass":               ("SLP-Vect",        "vectorize"),
    "LoopVectorizePass":               ("Loop-Vect",       "vectorize"),
    "LoopUnrollPass":                  ("Loop-Unroll",     "loop"),
    "LoopRotatePass":                  ("Loop-Rotate",     "loop"),
    "SimpleLoopUnswitchPass":          ("Loop-Unswitch",   "loop"),
    "LoopDistributePass":              ("Loop-Dist",       "loop"),
    "LoopFlattenPass":                 ("Loop-Flatten",    "loop"),
    "LoopInterchangePass":             ("Loop-Interchange","loop"),
    "LICMPass":                        ("LICM",            "memory"),
    "LoopLoadEliminationPass":         ("Loop-Load-Elim",  "memory"),
    "DSEPass":                         ("DSE",             "memory"),
    "MemCpyOptPass":                   ("MemCpyOpt",       "memory"),
    "GVN":                             ("GVN",             "scalar"),
    "InstCombinePass":                 ("InstCombine",     "scalar"),
    "JumpThreadingPass":               ("Jump-Thread",     "scalar"),
    "SROA":                            ("SROA",            "scalar"),
    "EarlyCSEPass":                    ("Early-CSE",       "scalar"),
    "SCCPPass":                        ("SCCP",            "scalar"),
    "InlinerPass":                     ("Inliner",         "inline"),
    "AlwaysInlinerPass":               ("Always-Inline",   "inline"),
    "LoopSimplifyPass":                ("Loop-Simplify",   "loop"),
    "IndVarSimplifyPass":              ("IndVar-Simp",     "loop"),
    "LoopIdiomRecognizePass":          ("Loop-Idiom",      "loop"),
    "LoopDeletionPass":                ("Loop-Delete",     "loop"),
    "ReassociatePass":                 ("Reassociate",     "scalar"),
    "CorrelatedValuePropagationPass":  ("CVP",             "scalar"),
    "ADCEPass":                        ("ADCE",            "scalar"),
    "BDCEPass":                        ("BDCE",            "scalar"),
    "ConstraintEliminationPass":       ("Constraint-Elim", "scalar"),
}


# ── Event extraction ──────────────────────────────────────────────────────────

def extract_pass_events(opt_path: str, ir_path: str,
                        kernel_name: str, timeout: int = 120) -> list:
    """
    Run `opt -passes=default<O3> -debug-pass-manager -disable-output <ir>`
    and parse "Running pass: X on Y" lines.

    Returns list of dicts:
      {pass, target, is_kernel, is_loop, order}
    ordered by execution.
    """
    cmd = [opt_path, "-passes=default<O3>",
           "-debug-pass-manager", "-disable-output", ir_path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, errors="replace")
    except Exception:
        return []

    pat = re.compile(r"Running pass:\s+(\S+)\s+on\s+(.+)")
    events, order = [], 0
    for line in r.stderr.splitlines():
        m = pat.search(line)
        if not m:
            continue
        pass_name = m.group(1)
        target = m.group(2).strip()
        is_loop = "loop" in target.lower()
        is_kernel = (kernel_name in target or is_loop)
        events.append({
            "pass":      pass_name,
            "target":    target,
            "is_kernel": is_kernel,
            "is_loop":   is_loop,
            "order":     order,
        })
        order += 1
    return events


# ── Graph building ────────────────────────────────────────────────────────────

def build_graph(events: list, kernel_name: str,
                remarks_by_pass: dict = None,
                ir_diffs: dict = None) -> dict:
    """
    Build a graph representation from pass events.

    Args:
      ir_diffs: optional dict from src.ir_diff.check_passes_ir_changes()
                {pass_name: {changed, skipped, delta_instr, delta_vec, ...}}

    Returns:
      nodes: list of {name, label, category, color, runs, missed, passed,
                      ir_changed, ir_skipped, ir_delta_vec, ir_delta_instr}
      edges: list of {src, dst}  (sequential execution order, deduplicated)
      stats: dict
    """
    kernel_events = [e for e in events if e["is_kernel"]]
    if not kernel_events:
        # Fall back to ALL events if we can't isolate kernel
        kernel_events = events

    # Count runs per pass
    run_count: dict = defaultdict(int)
    for e in kernel_events:
        run_count[e["pass"]] += 1

    # Build ordered node list (first appearance order)
    nodes: list = []
    seen: set = set()
    for e in kernel_events:
        p = e["pass"]
        if p in seen:
            continue
        seen.add(p)
        label, cat = _PASS_META.get(p, (p.replace("Pass", ""), "other"))
        remarks = (remarks_by_pass or {}).get(p, [])
        missed = sum(1 for r in remarks if r.get("type") == "missed")
        passed = sum(1 for r in remarks if r.get("type") == "passed")

        # IR diff data (from per-pass standalone opt runs)
        ir_info = (ir_diffs or {}).get(p, {})
        ir_changed  = ir_info.get("changed",  None)   # None = no data
        ir_skipped  = ir_info.get("skipped",  None)
        ir_dv       = ir_info.get("delta_vec",   0)
        ir_di       = ir_info.get("delta_instr", 0)

        nodes.append({
            "name":         p,
            "label":        label,
            "category":     cat,
            "color":        _CAT_COLOR.get(cat, _CAT_COLOR["other"]),
            "runs":         run_count[p],
            "missed":       missed,
            "passed":       passed,
            "ir_changed":   ir_changed,
            "ir_skipped":   ir_skipped,
            "ir_delta_vec": ir_dv,
            "ir_delta_instr": ir_di,
        })

    # Build edges (sequential execution, deduplicated)
    edge_set: set = set()
    edges: list = []
    prev = None
    for e in kernel_events:
        p = e["pass"]
        if prev and prev != p and (prev, p) not in edge_set:
            edge_set.add((prev, p))
            edges.append({"src": prev, "dst": p})
        prev = p

    stats = {
        "total_events":       len(kernel_events),
        "unique_passes":      len(nodes),
        "passes_with_misses": sum(1 for n in nodes if n["missed"] > 0),
        "total_misses":       sum(n["missed"] for n in nodes),
    }
    return {"nodes": nodes, "edges": edges, "stats": stats}


# ── DOT rendering ─────────────────────────────────────────────────────────────

def render_dot(graph: dict, kernel_name: str,
               dot_path: str, title: str = None) -> str:
    """
    Write a graphviz DOT file and attempt to render PNG + SVG.
    Returns the DOT string.
    """
    nodes = graph["nodes"]
    edges = graph["edges"]
    title = title or f"LLVM O3 Pass Pipeline — {kernel_name}"

    lines = [
        f'digraph "passes_{kernel_name}" {{',
        f'  label="{title}";',
        '  labelloc=t; labeljust=c;',
        '  fontname="Helvetica"; fontsize=13;',
        '  rankdir=LR;',
        '  node [fontname="Helvetica", fontsize=9, shape=box,'
        '        style="filled,rounded", margin="0.15,0.08"];',
        '  edge [fontname="Helvetica", fontsize=7, color="#555555"];',
        '',
        '  // ── Legend ──',
        '  subgraph cluster_legend {',
        '    label="Category"; style=dashed; fontsize=9;',
    ]
    for cat, color in _CAT_COLOR.items():
        lines.append(f'    _L_{cat} [label="{cat}", fillcolor="{color}"];')
    lines.append('  }')
    lines.append('')

    # Nodes
    for n in nodes:
        lbl = n["label"]
        if n["runs"] > 1:
            lbl += f"\\n×{n['runs']}"
        if n["missed"] > 0:
            lbl += f"\\n⚠ {n['missed']} missed"

        # IR diff annotation: show delta when the pass actually fired
        ir_changed  = n.get("ir_changed")
        ir_skipped  = n.get("ir_skipped")
        if ir_changed is True:
            dv = n.get("ir_delta_vec",   0)
            di = n.get("ir_delta_instr", 0)
            if dv != 0 or di != 0:
                lbl += f"\\n✓ fired: Δvec={dv:+d} Δinstr={di:+d}"
            else:
                lbl += "\\n✓ fired"

        # Border color priority: missed remarks (red) > IR fired (green) > default
        fill_color = n["color"]
        if ir_changed is False and ir_skipped is False:
            # Ran but was a no-op: grey fill to visually de-emphasize
            fill_color = "#CCCCCC"

        if n["missed"] > 0:
            border = ', color="#CC0000", penwidth=2.0'
        elif ir_changed is True:
            border = ', color="#008800", penwidth=2.5'
        else:
            border = ""

        lines.append(
            f'  "{n["name"]}" [label="{lbl}", fillcolor="{fill_color}"{border}];'
        )

    lines.append('')

    # Edges
    for e in edges:
        lines.append(f'  "{e["src"]}" -> "{e["dst"]}";')

    lines.append('}')
    dot_str = "\n".join(lines)

    Path(dot_path).parent.mkdir(parents=True, exist_ok=True)
    with open(dot_path, "w") as f:
        f.write(dot_str)

    # Try graphviz render
    for fmt, ext in (("png", ".png"), ("svg", ".svg")):
        out = dot_path.replace(".dot", ext)
        try:
            subprocess.run(["dot", f"-T{fmt}", dot_path, "-o", out],
                           capture_output=True, timeout=20)
        except Exception:
            pass

    return dot_str


# ── LLM-friendly text summary ─────────────────────────────────────────────────

def pass_pipeline_summary(graph: dict) -> str:
    """
    Return a compact multi-line text summary of the pass pipeline
    suitable for inclusion in an LLM prompt.
    """
    stats = graph["stats"]
    lines = [
        f"Pass pipeline: {stats['unique_passes']} unique passes, "
        f"{stats['total_events']} total executions, "
        f"{stats['passes_with_misses']} passes with missed remarks "
        f"({stats['total_misses']} total misses).",
        "",
        "Execution order (left→right, ⚠=has missed remarks):",
    ]
    row = []
    for n in graph["nodes"]:
        tag = " ⚠" if n["missed"] > 0 else ""
        row.append(f"{n['label']}{tag}")
    lines.append("  " + " → ".join(row))

    # Highlight passes with misses
    missed_passes = [n for n in graph["nodes"] if n["missed"] > 0]
    if missed_passes:
        lines.append("")
        lines.append("Passes with missed optimization remarks:")
        for n in missed_passes:
            lines.append(f"  {n['name']}: {n['missed']} missed, "
                         f"{n['passed']} succeeded")

    # IR modification summary (from per-pass standalone opt runs)
    nodes_with_diff = [n for n in graph["nodes"] if n.get("ir_changed") is not None]
    if nodes_with_diff:
        lines.append("")
        lines.append("Per-pass IR modification (FIRED=actually changed IR, no-op=ran but did nothing):")
        for n in nodes_with_diff:
            if n["ir_skipped"]:
                lines.append(f"  {n['name']}: skipped (analysis dependency)")
            elif n["ir_changed"]:
                dv = n.get("ir_delta_vec",   0)
                di = n.get("ir_delta_instr", 0)
                lines.append(f"  {n['name']}: FIRED  (Δvec={dv:+d}, Δinstr={di:+d})")
            else:
                lines.append(f"  {n['name']}: no-op")

    return "\n".join(lines)


# ── Top-level entry point ─────────────────────────────────────────────────────

def generate_pass_graph(opt_path: str, ir_path: str, kernel_name: str,
                        output_dir: str = "outputs",
                        remarks_by_pass: dict = None,
                        ir_diffs: dict = None) -> dict:
    """
    Full pipeline: extract → build → render.

    Returns graph dict with added keys:
      dot_path, png_path (if rendered), summary (str)
    """
    events = extract_pass_events(opt_path, ir_path, kernel_name)
    if not events:
        return {}

    graph = build_graph(events, kernel_name, remarks_by_pass, ir_diffs=ir_diffs)

    dot_path = str(Path(output_dir) / f"{kernel_name}_pass_graph.dot")
    render_dot(graph, kernel_name, dot_path)

    graph["dot_path"] = dot_path
    graph["png_path"] = dot_path.replace(".dot", ".png")
    graph["svg_path"] = dot_path.replace(".dot", ".svg")
    graph["summary"]  = pass_pipeline_summary(graph)
    return graph
