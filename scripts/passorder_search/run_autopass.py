#!/usr/bin/env python3
"""Faithful(er) reproduction of AutoPass (arxiv 2606.20373) -- "Evidence-
Guided LLM Agents for Compiler Performance Tuning" -- as the PO baseline in
comet's ablation study, replacing the earlier `run_one.py` (which was a
naive "guess a pass order blind, no compiler evidence, no rollback" strawman
that measured 0.2x-0.7x and was never a real approximation of the paper's
method).

Implements the paper's four-agent pipeline as described in the paper text
(exact formulas/prompts/pass catalog are NOT disclosed in the paper -- every
place this implementation had to fill a gap is commented explicitly):

  1. Score Agent    -- ranks call-graph-reachable functions by a proxy for
                        Table 2's features (#blocks, #loops, #calls,
                        #condbranch) to pick the optimization target. NOTE:
                        our build pipeline (measure_lib.compile_with_pass_order)
                        applies the chosen pass sequence across the WHOLE
                        kernel.c compilation unit (there is no llvm-link on
                        this toolchain to isolate one function's IR), so this
                        agent's practical role here is to focus the Analysis
                        Agent's remarks on the right function, not to scope
                        which IR the passes run over.
  2. Analysis Agent -- reuses comet's own -fsave-optimization-record=yaml
                        remarks extraction (src/remarks.py, already built for
                        the source-rewrite conditions) + an LLM call that
                        produces a normalized JSON summary of missed-optimization
                        opportunities, mirroring the paper's "-Rpass/-Rpass-missed
                        + semantic hints -> JSON summary" description.
  3. Reasoning Agent -- DeepSeek proposes the next full pass sequence (from
                        the 74-pass catalog in pass_list_autopass.py) given
                        the Analysis Agent's JSON + history of prior
                        (sequence, speedup) results. Includes a deterministic
                        repair step (difflib nearest-match) for any
                        hallucinated pass name, as the paper describes.
  4. Evaluation Agent -- compiles + measures the candidate; STRICTLY accepts
                        only if faster than the current best P* (paper: "accepts
                        the candidate only if t(P(t)) < t(P*)"), else rolls
                        back to P* for the next round's starting point.

Round budget: 3 (matches the paper's primary reported "R3" configuration,
which reports 1.04x-1.15x geomean over -O3 -- NOT comet's own 9-round
budget used elsewhere in this ablation study; using 9 rounds here would no
longer be reproducing what the paper's headline numbers describe).

Usage: run_autopass.py <program_rel_path> <scratch_dir> [rounds=3] [confirm_runs=3] [pin_cpu]
"""
import sys, json, uuid, difflib
from pathlib import Path

sys.path.insert(0, "/home/hanning/comet/scripts/passorder_search")
from measure_lib import compile_with_pass_order, time_binary, correctness_check  # noqa: E402
from pass_list_autopass import CANONICAL_PASSES_74  # noqa: E402
from llm_client import ask_json  # noqa: E402

sys.path.insert(0, "/home/hanning/comet")
from optimize import confirm_result_external  # noqa: E402
from src.hotspot import rank_all_reachable  # noqa: E402
from src.remarks import extract_rich_remarks_yaml, format_rich_remarks_for_source_prompt  # noqa: E402
import yaml  # noqa: E402

_cfg = yaml.safe_load(open("/home/hanning/comet/configs/config.yaml").read())
CLANG = _cfg["compiler"]["clang_path"]

def _pass_base(p: str) -> str:
    """"loop-mssa(licm)" -> "licm", "loop-mssa(lnicm)" -> "lnicm" -- both
    wrap distinct inner passes under the same "loop-mssa" adaptor, so
    splitting on the outer name alone (as compile_with_pass_order's syntax
    would suggest) collides the two into one dict key. Use the innermost
    name as the lookup key instead; anything without "(...)" wrapping keys
    on itself as before."""
    if p.startswith("loop-mssa(") and p.endswith(")"):
        return p[len("loop-mssa("):-1]
    return p.split("(")[0]


VALID_PASS_SET = {_pass_base(p) for p in CANONICAL_PASSES_74}
VALID_PASS_BY_BASE = {_pass_base(p): p for p in CANONICAL_PASSES_74}


# ---------------------------------------------------------------------------
# 1. Score Agent
# ---------------------------------------------------------------------------
def score_agent(kernel_name: str, driver_text: str, utils_text: "str | None") -> dict:
    """Rank call-graph-reachable functions by a textual proxy for the paper's
    Table 2 features (#blocks, #loops, #calls, #condbranch). The paper does
    not disclose how these combine into a single score -- this weighting
    (blocks_proxy + 2*loops + calls + condbranch) is OUR OWN choice, not
    taken from the paper. Returns the top candidate and its feature dict."""
    import re
    candidates = rank_all_reachable(kernel_name, driver_text, utils_text)
    scored = []
    for c in candidates:
        body = c.get("body", "") or ""
        blocks = body.count("{")  # proxy: compound-statement nesting depth
        loops = len(re.findall(r"\b(for|while)\s*\(", body))
        calls = len(re.findall(r"\b[A-Za-z_]\w*\s*\(", body))
        condbranch = (len(re.findall(r"\bif\s*\(", body))
                      + body.count("&&") + body.count("||")
                      + len(re.findall(r"\bswitch\s*\(", body)))
        score = blocks + 2 * loops + calls + condbranch
        scored.append({"name": c["name"], "blocks": blocks, "loops": loops,
                        "calls": calls, "condbranch": condbranch, "score": score})
    scored.sort(key=lambda x: -x["score"])
    return scored[0] if scored else {"name": kernel_name, "score": 0}


# ---------------------------------------------------------------------------
# 2. Analysis Agent
# ---------------------------------------------------------------------------
ANALYSIS_SYSTEM = """You are the Analysis Agent in an LLVM compiler-tuning pipeline. You are given \
LLVM optimization remarks (-Rpass/-Rpass-missed output: which optimizations fired, which were \
missed and why) for one target function, plus its name. Produce a normalized JSON summary of \
optimization opportunities the Reasoning Agent (which picks the next LLVM pass ORDER to try) \
should act on.
Respond with ONLY a JSON object:
{"semantic_hint": "<1 sentence on what kind of computation this function name/shape suggests>",
 "missed_categories": ["vectorization"|"memory"|"loop-structure"|"redundancy"|"inlining", ...],
 "top_opportunities": ["<short actionable note>", ...],
 "reasoning": "<1-2 sentences>"}"""


def analysis_agent(kernel_c: str, utils: str, source_dir: str, target_func: str) -> dict:
    rich = extract_rich_remarks_yaml(CLANG, kernel_c, Path(utils), Path(source_dir), target_func)
    remarks_text = format_rich_remarks_for_source_prompt(rich) if rich else "(no remarks captured)"
    user = f"Target function: {target_func}\n\nRemarks:\n{remarks_text[:4000]}"
    reply = ask_json(ANALYSIS_SYSTEM, user, max_tokens=500)
    if not reply:
        reply = {"semantic_hint": "", "missed_categories": [], "top_opportunities": [],
                  "reasoning": "(analysis LLM call failed or returned empty)"}
    return reply


# ---------------------------------------------------------------------------
# 3. Reasoning Agent
# ---------------------------------------------------------------------------
REASONING_SYSTEM = f"""You are the Reasoning Agent in an LLVM compiler-tuning pipeline \
(aarch64, LLVM 21, clang/opt/llc). You do NOT edit source code and do NOT tune pass PARAMETERS \
-- only the ORDER (and which subset, and optional repeats) of passes applied to one kernel \
function, from this fixed 74-pass catalog of valid `opt -passes=` names:

{", ".join(CANONICAL_PASSES_74)}

`mem2reg` is always run first automatically -- do not include it yourself.

You are given: (1) the Analysis Agent's JSON summary of missed-optimization opportunities for \
the target function, (2) history of (pass sequence -> measured speedup vs -O3) from previous \
rounds. A candidate is only KEPT if it beats the best speedup found so far -- otherwise the \
system rolls back and you see the rejection in history. Use the analysis summary and history to \
propose a pass sequence 15-40 entries long (repeats/subsets allowed) that directly targets the \
listed missed opportunities.

Respond with ONLY JSON: {{"passes": ["name1", "name2", ...], "reasoning": "..."}}"""


def _repair_pass_name(name: str) -> "str | None":
    """Deterministic repair for a hallucinated/invalid pass token: map to the
    nearest valid catalog name by string similarity (paper: 'mapping an
    invalid pass token to the most similar valid pass', exact metric/threshold
    undisclosed -- difflib SequenceMatcher ratio, cutoff 0.6, is our choice)."""
    base = _pass_base(name)
    if base in VALID_PASS_BY_BASE:
        return VALID_PASS_BY_BASE[base]
    close = difflib.get_close_matches(base, VALID_PASS_SET, n=1, cutoff=0.6)
    return VALID_PASS_BY_BASE[close[0]] if close else None


def reasoning_agent(analysis_json: dict, history: list, round_num: int, rounds: int,
                     kernel_name: str) -> list:
    lines = [f"Kernel: {kernel_name}. Round {round_num}/{rounds}.",
              f"Analysis summary: {json.dumps(analysis_json)}"]
    if not history:
        lines.append("This is the first attempt -- propose an initial pass sequence "
                      "targeting the analysis summary's top opportunities.")
    else:
        lines.append("History (sequence -> result):")
        for h in history[-5:]:
            if h["accepted"]:
                lines.append(f"  ACCEPTED speedup={h['speedup']:.4f}x  passes={h['passes']}")
            else:
                lines.append(f"  REJECTED (worse than best) speedup={h['speedup']:.4f}x  passes={h['passes']}")
        best = max((h for h in history if h["accepted"]), key=lambda h: h["speedup"], default=None)
        if best:
            lines.append(f"Current best P*: {best['speedup']:.4f}x with {best['passes']}")
        lines.append("Propose the next sequence, aiming to beat P*.")
    reply = ask_json(REASONING_SYSTEM, "\n".join(lines), max_tokens=700)
    raw_passes = reply.get("passes", []) if isinstance(reply, dict) else []

    repaired = []
    for p in raw_passes:
        fixed = _repair_pass_name(p)
        if fixed:
            repaired.append(fixed)
    if not repaired:
        repaired = list(CANONICAL_PASSES_74)  # fallback: full catalog in canonical order
    return repaired


# ---------------------------------------------------------------------------
# 4. Evaluation Agent (strict accept-iff-better-than-P*, else rollback)
# ---------------------------------------------------------------------------
def evaluation_agent(kernel_c: str, utils: str, source_dir: str, passes: list,
                      baseline_ms: float, work_dir: Path, round_num: int,
                      pin_cpu: "int | None", ref_bin_dump: str,
                      correctness_mode: str) -> dict:
    """Compiles + times the candidate, AND checks correctness against
    ref_bin_dump before returning a speedup -- arbitrary pass reordering is
    not always semantics-preserving (some passes assume invariants normally
    established by earlier passes in the standard -O3 sequence), so a
    candidate that is merely "fast" but silently wrong must never become
    P*. Rejecting incorrect candidates per-round (not just once at the very
    end) also stops the whole run's exploration budget from being spent
    chasing a candidate that fails at finalize time anyway."""
    trial_bin = str(work_dir / f"trial_{round_num}_{uuid.uuid4().hex[:6]}")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, passes, trial_bin,
                                       work_dir=str(work_dir))
    if not ok:
        return {"ok": False, "error": err}

    trial_bin_dump = str(work_dir / f"trial_{round_num}_{uuid.uuid4().hex[:6]}_dump")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, passes, trial_bin_dump,
                                       work_dir=str(work_dir), output_macro="POLYBENCH_DUMP_ARRAYS")
    if not ok:
        return {"ok": False, "error": f"DUMP_ARRAYS build: {err}"}
    correct, cerr = correctness_check(ref_bin_dump, trial_bin_dump, correctness_mode)
    if not correct:
        return {"ok": False, "error": f"incorrect: {cerr}"}

    tok, ms = time_binary(trial_bin, runs=1, pin_cpu=pin_cpu)
    if not tok:
        return {"ok": False, "error": "run failed/crashed"}
    speedup = baseline_ms / ms if ms > 0 else 0.0
    return {"ok": True, "speedup": speedup}


def main():
    program_rel = sys.argv[1]
    scratch_dir = Path(sys.argv[2])
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3   # R3, matches paper's headline config
    confirm_runs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    pin_cpu = int(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None

    baseline = json.loads((scratch_dir / "baseline.json").read_text())
    # Matches optimize.py's own convention (f"kernel_{name...}") -- every
    # PolyBench/cBench kernel function in this corpus is named kernel_<stem>,
    # not just <stem>. Using the bare stem here (as the old run_one.py did)
    # makes rank_all_reachable/extract_rich_remarks_yaml silently find nothing.
    kernel_name = f"kernel_{Path(program_rel).stem.replace('-', '_')}"
    utils, source_dir = baseline["utils"], baseline["source_dir"]
    baseline_ms = baseline["baseline_ms"]
    kernel_c = str(scratch_dir / "kernel.c")

    work_dir = scratch_dir / "work"
    work_dir.mkdir(exist_ok=True)

    driver_text = Path(kernel_c).read_text(errors="replace")
    utils_text = None
    utils_polybench_c = Path(utils) / "polybench.c"
    if utils_polybench_c.exists():
        utils_text = utils_polybench_c.read_text(errors="replace")

    # 1. Score Agent
    target = score_agent(kernel_name, driver_text, utils_text)
    target_func = target.get("name", kernel_name)
    print(f"[Score Agent] target={target_func} features={target}")

    # 2. Analysis Agent
    analysis = analysis_agent(kernel_c, utils, source_dir, target_func)
    print(f"[Analysis Agent] {json.dumps(analysis, ensure_ascii=False)}")

    history = []
    best_passes, best_speedup = list(CANONICAL_PASSES_74), 1.0

    for round_num in range(1, rounds + 1):
        # 3. Reasoning Agent
        passes = reasoning_agent(analysis, history, round_num, rounds, kernel_name)

        # 4. Evaluation Agent
        result = evaluation_agent(kernel_c, utils, source_dir, passes, baseline_ms,
                                   work_dir, round_num, pin_cpu,
                                   baseline["ref_bin_dump"], baseline["correctness_mode"])
        if not result["ok"]:
            history.append({"accepted": False, "passes": passes, "speedup": 0.0,
                             "error": result["error"]})
            print(f"[round {round_num}/{rounds}] FAILED: {result['error'][:200]}")
            continue

        speedup = result["speedup"]
        accepted = speedup > best_speedup   # strict: t(P(t)) < t(P*)  <=>  speedup > best_speedup
        history.append({"accepted": accepted, "passes": passes, "speedup": speedup})
        tag = "ACCEPTED (new P*)" if accepted else "REJECTED, rollback to P*"
        print(f"[round {round_num}/{rounds}] speedup={speedup:.4f}x  {tag}  passes={passes}")
        if accepted:
            best_speedup, best_passes = speedup, passes
        # else: best_passes/best_speedup untouched -- this IS the rollback.

    (scratch_dir / "history.json").write_text(json.dumps(history, indent=1))
    (scratch_dir / "best.json").write_text(json.dumps(
        {"best_passes": best_passes, "best_speedup": best_speedup,
         "score_agent_target": target, "analysis_agent": analysis}, indent=1))

    # ── Finalize: rebuild P* into a clean binary, correctness check against
    # ref_bin_dump, then the same alternating-measurement confirmation every
    # other condition uses. ────────────────────────────────────────────────
    opt_bin = str(scratch_dir / "opt_bin")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, best_passes, opt_bin,
                                       work_dir=str(work_dir))
    result = {"program": program_rel, "baseline_ms": baseline_ms,
              "best_passes": best_passes, "explored_best_speedup": best_speedup,
              "score_agent_target": target, "analysis_agent": analysis}
    if not ok:
        result.update(status="compile_failed", error=err[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    opt_bin_dump = str(scratch_dir / "opt_bin_dump")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, best_passes, opt_bin_dump,
                                       work_dir=str(work_dir), output_macro="POLYBENCH_DUMP_ARRAYS")
    if not ok:
        result.update(status="compile_failed", error=("DUMP_ARRAYS build: " + err)[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    ref_bin_dump = baseline.get("ref_bin_dump", baseline["ref_bin"])
    correct, cerr = correctness_check(ref_bin_dump, opt_bin_dump, baseline["correctness_mode"])
    if not correct:
        result.update(status="incorrect", error=cerr, confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    confirm = confirm_result_external(baseline["ref_bin"], opt_bin, confirm_runs, pin_cpu)
    if not confirm.get("ok"):
        result.update(status="confirm_failed", confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    result.update(
        status="confirmed",
        confirmed_speedup=confirm["confirmed_speedup"],
        best_speedup=confirm["best_speedup"],
        n=confirm["n"],
        n_positive=confirm["n_positive"],
        significant=confirm["confirmed_speedup"] > 1.0,
        speedup_iqr=confirm["speedup_iqr"],
    )
    print(json.dumps(result, indent=1))
    (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
