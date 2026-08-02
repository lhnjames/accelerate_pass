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
  3. Reasoning Agent -- DeepSeek proposes the next pass sequence (from the
                        74-pass catalog in pass_list_autopass.py) AND the
                        numeric pass parameters to go with it (TUNABLE_PARAMS
                        in the same file -- the paper tunes these too; its
                        QSort trace study sets unroll_count/unroll_threshold/
                        inline_threshold/slp_threshold and then walks them
                        back after a regression), given the Analysis Agent's
                        JSON + history of prior (sequence, params, speedup)
                        results. Includes a deterministic repair step (difflib
                        nearest-match) for any hallucinated pass name, as the
                        paper describes; unknown PARAMETER names are dropped
                        rather than fuzzy-matched, since a mis-matched knob
                        would silently change an unrelated setting.
  4. Evaluation Agent -- compiles + measures the candidate; STRICTLY accepts
                        only if faster than the current best P* (paper: "accepts
                        the candidate only if t(P(t)) < t(P*)"), else rolls
                        back to P* for the next round's starting point.

Known deviation: the paper also feeds hardware counters (L1 misses, IPC)
back to the Evaluation Agent. perf is unavailable here -- perf_event_paranoid
is 4 on both measurement nodes and raising it is a host-wide security setting
this project should not change unilaterally -- so the Evaluation Agent sees
wall-clock time and correctness only.

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
from pass_list_autopass import CANONICAL_PASSES_74, TUNABLE_PARAMS  # noqa: E402
from llm_client import ask_json  # noqa: E402

sys.path.insert(0, "/home/hanning/comet")
from optimize import confirm_result_external  # noqa: E402
from src.hotspot import rank_all_reachable  # noqa: E402
from src.remarks import extract_rich_remarks_yaml, format_rich_remarks_for_source_prompt  # noqa: E402
import yaml  # noqa: E402

_cfg = yaml.safe_load(open("/home/hanning/comet/configs/config.yaml").read())
CLANG = _cfg["compiler"]["clang_path"]

# LLVM's stock -O3 pipeline, used as the initial P* (the thing candidates
# must beat). `opt -passes='default<O3>'` on the frontend's raw IR reproduces
# the -O3 baseline to within noise (measured 1.01x on gemm), so seeding P*
# with it means "no candidate was better than -O3" finalizes as an honest
# ~1.0x rather than as whatever the fallback catalog happens to measure.
O3_PIPELINE = "default<O3>"


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
(aarch64, LLVM 21, clang/opt/llc). You do NOT edit source code. You control (a) the ORDER \
(and which subset, and optional repeats) of passes applied to one kernel function, and (b) the \
numeric PARAMETERS of those passes. Passes come from this fixed 74-pass catalog of valid \
`opt -passes=` names:

{", ".join(CANONICAL_PASSES_74)}

`mem2reg` is always run first automatically -- do not include it yourself.

You are given: (1) the Analysis Agent's JSON summary of missed-optimization opportunities for \
the target function, (2) history of (pass sequence -> measured speedup vs -O3) from previous \
rounds. A candidate is only KEPT if it beats the best speedup found so far -- otherwise the \
system rolls back and you see the rejection in history. Use the analysis summary and history to \
propose a pass sequence 15-40 entries long (repeats/subsets allowed) that directly targets the \
listed missed opportunities.

You may ALSO set numeric pass parameters. Available parameters (suggested values shown; any \
integer is accepted, and you may omit any or all of them):

{chr(10).join(f"  {k}: {v}" for k, v in TUNABLE_PARAMS.items())}

Tuning these is often what actually moves performance -- e.g. raising unroll-threshold/unroll-count \
to expose more ILP in a hot loop, lowering slp-threshold to make SLP vectorization more eager, or \
raising inline-threshold to expose cross-call optimization. But over-aggressive values regress: \
excessive unrolling blows up I-cache and register pressure, and an overly permissive slp-threshold \
vectorizes code that is cheaper scalar. Read the history and walk values back when a round regressed.

Respond with ONLY JSON:
{{"passes": ["name1", "name2", ...], "params": {{"unroll-threshold": 600, ...}}, "reasoning": "..."}}"""


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


def _repair_params(raw: dict) -> list:
    """Turn the Reasoning Agent's {"unroll-threshold": 600, ...} into
    ["-unroll-threshold=600", ...], dropping anything not in TUNABLE_PARAMS
    or not integer-valued. Unknown parameter names are dropped rather than
    fuzzy-matched: a wrong pass NAME just means a different transform runs,
    but a wrong parameter name silently changes an unrelated knob, so the
    deterministic-repair story from the paper does not transfer here."""
    out = []
    for k, v in (raw or {}).items():
        key = str(k).lstrip("-")
        if key not in TUNABLE_PARAMS:
            continue
        try:
            out.append(f"-{key}={int(v)}")
        except (TypeError, ValueError):
            continue
    return out


_PREFLIGHT_LL = [None]   # cached tiny module, built once per process


def _preflight_module() -> "str | None":
    """A throwaway .ll with one FP loop and one integer loop, used to ask opt
    whether a proposed pipeline is even accepted before we spend a round
    compiling the real kernel with it."""
    if _PREFLIGHT_LL[0] is None:
        import tempfile, subprocess
        td = Path(tempfile.mkdtemp(prefix="autopass_preflight_"))
        c = td / "p.c"
        c.write_text("double a[256],b[256];\n"
                     "void f(int n){for(int i=0;i<n;i++) a[i]=b[i]*2.0+a[i];}\n"
                     "int g(int n){int s=0;for(int i=0;i<n;i++) s+=i*i; return s;}\n")
        ll = td / "p.ll"
        r = subprocess.run([CLANG, "-O3", "-Xclang", "-disable-llvm-passes",
                            "-S", "-emit-llvm", str(c), "-o", str(ll)],
                           capture_output=True)
        _PREFLIGHT_LL[0] = str(ll) if r.returncode == 0 else ""
    return _PREFLIGHT_LL[0] or None


def _opt_accepts(passes: list, params: list) -> tuple:
    """(ok, stderr) for `opt -passes=<passes> <params>` on the tiny module."""
    import subprocess
    ll = _preflight_module()
    if not ll or not passes:
        return True, ""     # can't pre-check -- let the real build decide
    r = subprocess.run([_cfg["compiler"]["opt_path"], "-passes=" + ",".join(passes),
                        *params, ll, "-o", "/dev/null"],
                       capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or "").strip()


def _first_line(err: str) -> str:
    """opt's first diagnostic line -- the part that says WHY, without the
    (very long) -passes= echo that follows."""
    for line in (err or "").splitlines():
        line = line.strip()
        if line:
            return line[:160]
    return "(no stderr)"


def validate_and_repair(passes: list, params: list) -> tuple:
    """Drop whatever `opt` refuses, so an unparseable proposal costs a pass,
    not the whole round.

    13% of the first full PO sweep's evaluation budget (19 of 147 rounds)
    was lost to opt rejecting a proposal, and four programs -- susan_smoothing,
    tiff2bw, dijkstra, security_sha -- lost ALL THREE rounds this way. Those
    programs were then recorded at 1.000x as if the search had honestly found
    nothing, when in fact it never got to run, which quietly biases the
    geomean toward 1.0.

    Individually every catalog entry and every tunable parameter is valid
    (each checked one at a time against opt-21), and so is the full catalog
    in canonical order, so the rejections come from specific combinations.
    Which ones is no longer recoverable from the first sweep: the round loop
    logged only error[:200], and the -passes= argument alone is longer than
    that, so opt's actual diagnostic was truncated away in every case (now
    fixed below). That LLVM's new-PM parser can reject a combination outright
    is easy to demonstrate regardless -- a bare `licm` is fatal ("LICM
    requires MemorySSA (loop-mssa)") while `loop-mssa(licm)` is fine.

    So rather than enumerate rules we cannot fully enumerate -- and which are
    LLVM-version specific and would rot anyway -- build the pipeline
    incrementally and keep only the prefix extensions opt actually accepts.
    O(n) opt invocations on a ~10-line module, far cheaper than one wasted
    round on the real kernel, and it degrades gracefully on any future LLVM.
    """
    ok, _ = _opt_accepts(passes, params)
    if ok:
        return passes, params, []

    dropped = []

    # Parameters FIRST, against a pipeline known to be valid. opt rejects an
    # unknown flag before it ever runs a pass, so a single bad parameter makes
    # every candidate prefix fail -- repairing passes first would then blame
    # the passes and throw away the whole (perfectly good) sequence.
    kept_params = []
    for prm in params:
        good, err = _opt_accepts([O3_PIPELINE], kept_params + [prm])
        if good:
            kept_params.append(prm)
        else:
            dropped.append((prm, _first_line(err)))

    # Then the sequence, now that the parameters can't cause false blame.
    kept = []
    for p in passes:
        good, err = _opt_accepts(kept + [p], kept_params)
        if good:
            kept.append(p)
        else:
            dropped.append((p, _first_line(err)))

    if not kept:
        # Nothing survived -- fall back to the stock -O3 pipeline so the
        # round still measures something meaningful instead of nothing.
        return [O3_PIPELINE], kept_params, dropped
    return kept, kept_params, dropped


def reasoning_agent(analysis_json: dict, history: list, round_num: int, rounds: int,
                     kernel_name: str) -> tuple:
    lines = [f"Kernel: {kernel_name}. Round {round_num}/{rounds}.",
              f"Analysis summary: {json.dumps(analysis_json)}"]
    if not history:
        lines.append("This is the first attempt -- propose an initial pass sequence "
                      "targeting the analysis summary's top opportunities.")
    else:
        lines.append("History (sequence -> result):")
        for h in history[-5:]:
            tag = "ACCEPTED" if h["accepted"] else "REJECTED (worse than best)"
            pstr = f"  params={h['params']}" if h.get("params") else ""
            lines.append(f"  {tag} speedup={h['speedup']:.4f}x  passes={h['passes']}{pstr}")
        best = max((h for h in history if h["accepted"]), key=lambda h: h["speedup"], default=None)
        if best:
            lines.append(f"Current best P*: {best['speedup']:.4f}x with {best['passes']} "
                          f"params={best.get('params') or '{}'}")
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
    params = _repair_params(reply.get("params") if isinstance(reply, dict) else None)

    # Name-level repair above only guarantees each token exists; the pipeline
    # as a whole can still be rejected by opt. Pre-flight it so a bad ordering
    # costs the offending pass rather than the entire round.
    repaired, params, dropped = validate_and_repair(repaired, params)
    if dropped:
        print(f"[round {round_num}/{rounds}] pre-flight dropped {len(dropped)} rejected item(s):")
        for item, reason in dropped:
            print(f"    {item!r}: {reason}")
    return repaired, params


# ---------------------------------------------------------------------------
# 4. Evaluation Agent (strict accept-iff-better-than-P*, else rollback)
# ---------------------------------------------------------------------------
def evaluation_agent(kernel_c: str, utils: str, source_dir: str, passes: list,
                      baseline_ms: float, work_dir: Path, round_num: int,
                      pin_cpu: "int | None", ref_bin_dump: str,
                      correctness_mode: str, ref_bin: str,
                      opt_params: list) -> dict:
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
                                       work_dir=str(work_dir), opt_params=opt_params)
    if not ok:
        return {"ok": False, "error": err}

    trial_bin_dump = str(work_dir / f"trial_{round_num}_{uuid.uuid4().hex[:6]}_dump")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, passes, trial_bin_dump,
                                       work_dir=str(work_dir), output_macro="POLYBENCH_DUMP_ARRAYS",
                                       opt_params=opt_params)
    if not ok:
        return {"ok": False, "error": f"DUMP_ARRAYS build: {err}"}
    correct, cerr = correctness_check(ref_bin_dump, trial_bin_dump, correctness_mode)
    if not correct:
        return {"ok": False, "error": f"incorrect: {cerr}"}

    # PAIRED measurement: re-time the reference binary right next to the
    # candidate rather than dividing by the baseline_ms recorded back in
    # prepare_task. Those two measurements can be hours and several
    # concurrent workers apart -- on a loaded node the same ref_bin measured
    # 139ms at prepare time and 618ms during a round, which would turn an
    # honest 0.82x candidate into a reported 0.18x purely from machine load.
    # Pairing cancels the drift, the same way confirm_result_external does
    # for the final number.
    tok_ref, ref_ms = time_binary(ref_bin, runs=1, pin_cpu=pin_cpu)
    tok, ms = time_binary(trial_bin, runs=1, pin_cpu=pin_cpu)
    if not tok:
        return {"ok": False, "error": "run failed/crashed"}
    if not tok_ref or ref_ms <= 0:
        ref_ms = baseline_ms   # fall back to the recorded baseline
    speedup = ref_ms / ms if ms > 0 else 0.0
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
    # P* starts as LLVM's own -O3 pipeline at speedup 1.0 -- that IS the
    # baseline every candidate must beat, and it's what "rollback" means in
    # the paper (accept only if t(P) < t(P*); otherwise keep what you had).
    #
    # Previously this was seeded with the raw 74-pass catalog at an ASSUMED
    # speedup of 1.0, which was never measured and is in fact far slower than
    # -O3. When no round beat 1.0x (the common case), finalize would then
    # compile that unmeasured catalog and report its real speed -- which is
    # how tasks that genuinely found no improvement got recorded as 0.2x-0.6x
    # "results" instead of an honest 1.0x "no improvement over -O3".
    best_passes, best_speedup, best_params = [O3_PIPELINE], 1.0, []

    for round_num in range(1, rounds + 1):
        # 3. Reasoning Agent
        passes, params = reasoning_agent(analysis, history, round_num, rounds, kernel_name)

        # 4. Evaluation Agent
        result = evaluation_agent(kernel_c, utils, source_dir, passes, baseline_ms,
                                   work_dir, round_num, pin_cpu,
                                   baseline["ref_bin_dump"], baseline["correctness_mode"],
                                   baseline["ref_bin"], params)
        if not result["ok"]:
            history.append({"accepted": False, "passes": passes, "params": params,
                             "speedup": 0.0, "error": result["error"]})
            # Log the error in full. The 200-char cap used previously truncated
            # opt's actual diagnostic away -- every failure in the first sweep
            # showed only the (long) -passes= argument, leaving no way to tell
            # afterwards WHY the pipeline was rejected.
            print(f"[round {round_num}/{rounds}] FAILED: {result['error']}")
            continue

        speedup = result["speedup"]
        accepted = speedup > best_speedup   # strict: t(P(t)) < t(P*)  <=>  speedup > best_speedup
        history.append({"accepted": accepted, "passes": passes, "params": params,
                         "speedup": speedup})
        tag = "ACCEPTED (new P*)" if accepted else "REJECTED, rollback to P*"
        print(f"[round {round_num}/{rounds}] speedup={speedup:.4f}x  {tag}  "
              f"passes={passes} params={params}")
        if accepted:
            best_speedup, best_passes, best_params = speedup, passes, params
        # else: best_passes/best_speedup untouched -- this IS the rollback.

    (scratch_dir / "history.json").write_text(json.dumps(history, indent=1))
    (scratch_dir / "best.json").write_text(json.dumps(
        {"best_passes": best_passes, "best_params": best_params,
         "best_speedup": best_speedup,
         "score_agent_target": target, "analysis_agent": analysis}, indent=1))

    # ── Finalize: rebuild P* into a clean binary, correctness check against
    # ref_bin_dump, then the same alternating-measurement confirmation every
    # other condition uses. ────────────────────────────────────────────────
    opt_bin = str(scratch_dir / "opt_bin")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, best_passes, opt_bin,
                                       work_dir=str(work_dir), opt_params=best_params)
    result = {"program": program_rel, "baseline_ms": baseline_ms,
              "best_passes": best_passes, "best_params": best_params,
              "explored_best_speedup": best_speedup,
              # True when no round beat -O3, so P* is still the stock -O3
              # pipeline. Such a task is an honest "AutoPass found nothing
              # better here" (expected to confirm at ~1.0x), NOT a regression.
              "no_improvement_over_O3": best_passes == [O3_PIPELINE] and not best_params,
              "score_agent_target": target, "analysis_agent": analysis}
    if not ok:
        result.update(status="compile_failed", error=err[:1000],
                       confirmed_speedup=1.0, significant=False)
        print(json.dumps(result, indent=1))
        (scratch_dir / "result.json").write_text(json.dumps(result, indent=1))
        return

    opt_bin_dump = str(scratch_dir / "opt_bin_dump")
    ok, err = compile_with_pass_order(kernel_c, utils, source_dir, best_passes, opt_bin_dump,
                                       work_dir=str(work_dir), output_macro="POLYBENCH_DUMP_ARRAYS",
                                       opt_params=best_params)
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
