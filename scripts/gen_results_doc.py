#!/usr/bin/env python3
"""Regenerate the results document from queue + node state.

Re-runnable at any point -- mid-sweep for a progress snapshot, again at the end
for the final table. It never reads a previous version of the document, so the
doc cannot drift from the data.

    python3 scripts/gen_results_doc.py                    # live queue
    python3 scripts/gen_results_doc.py --archive          # the pre-fix sweep
    python3 scripts/gen_results_doc.py --state P --logdir D -o out.md

Three things it does that hand-extraction kept getting wrong:

1. RESOLVE EACH TASK TO THE NODE THE QUEUE SAYS RAN IT. The same task id leaves
   a log on any node that ever ran it, and re-runs routinely land on the other
   machine, so scanning logs directly silently mixes results from a superseded
   sweep into the totals.

2. ONE ROW PER (CONDITION, PROGRAM), the most recently finished. A re-run is
   enqueued under a new id so its log doesn't clobber the superseded one, which
   means a cell can appear twice; counting both double-weights that program AND
   averages a fixed-harness result together with the one it replaced.

3. REPORT THE MEDIAN CONFIRMATION RUN. Data produced before 2026-08-02 stored
   final_speedup = max of the n paired runs; this recomputes from the stored
   confirmed_median so old and new sweeps are on one scale, and re-derives
   `significant` under the current rule (IQR entirely above 1.0 AND every run
   positive) rather than trusting the stored flag.

It also labels every row with a validity verdict, because several defects found
on 2026-07-31/08-02 invalidate specific subsets rather than the whole dataset.
"""
import argparse, calendar, json, math, subprocess, sys, datetime
import statistics as st
from collections import Counter
from pathlib import Path

QUEUE_HOST = "oracle4"
LIVE_STATE = "/home/hanning/comet_queue/state.json"
ARCHIVE_STATE = "/home/hanning/comet_queue/state_archive_20260802_prefix.json"
NODES = {"dgx-spark-a": "dss-dgx-a", "dgx-spark-b": "dss-dgx-b"}
COMET = "/home/hanning/comet"
LIVE_LOGDIR = COMET + "/logs_queue_run_v2"
ARCHIVE_LOGDIR = COMET + "/logs_queue_run_v2_archive_20260802_prefix"

NAMES = {
    "c1": "① rewrite-only（禁用编译器反馈，每步强制 rewrite_source）",
    "c2": "② no-compiler-feedback（自由选动作，屏蔽编译器反馈）",
    "c3": "③ full system（自由选动作 + 完整编译器反馈）",
    "c4": "④ params-only（每步强制 try_flags）",
    "oc": "OC — OpenCode + DeepSeek 外部 CLI agent baseline",
    "po": "PO — AutoPass (arXiv 2606.20373) 四-agent 复现 baseline",
}
ORDER = ["c1", "c2", "c3", "c4", "oc", "po"]
SHORT = {c: NAMES[c].split("（")[0].split(" —")[0] for c in ORDER}

# ── Validity rules ──────────────────────────────────────────────────────────
# Programs whose correctness tier changed on 2026-08-02 from `numeric`
# (1e-4 RELATIVE tolerance) to `hash` (byte-exact). On these the old gate was
# strictly weaker than it should have been -- telecom_crc32 prints a ~4e9
# checksum, where a 1e-4 relative tolerance admits a ±400000 error -- so any
# result that ADOPTED a change was never validly checked.
TIER_CHANGED = {
    "automotive_qsort1", "bzip2_decode", "network_dijkstra", "network_patricia",
    "office_stringsearch2", "telecom_crc32", "security_sha",
    "floyd-warshall", "nussinov", "correlation",
}
# Two orphaned (PPID 1) harness processes sat on the pinned measurement core.
ORPHAN = {
    "dgx-spark-a": (calendar.timegm((2026, 7, 30, 18, 1, 0, 0, 0, 0)),
                    calendar.timegm((2026, 7, 31, 17, 30, 0, 0, 0, 0))),
    "dgx-spark-b": (calendar.timegm((2026, 7, 30, 20, 8, 0, 0, 0, 0)),
                    calendar.timegm((2026, 7, 31, 17, 30, 0, 0, 0, 0))),
}
# InstCombine's fixpoint verifier aborted 19 of the first PO sweep's 147
# rounds, and four programs lost all three, so every pre-fix PO number is
# biased toward 1.0 on top of the orphan contamination.
PO_PREFIX_DISCARDED = True

# Independently re-measured on an idle core, 15 alternating pairs (9 for
# susan_smoothing), byte-exact output verified in every case.
VERIFIED = [
    ("telecom_crc32", "c4", 1.6346, 1.0161, "[0.979, 1.051]", "8/15", "0.39 ms"),
    ("security_rijndael_decode", "c4", 1.4484, 1.0080, "[0.991, 1.031]", "9/15", "0.43 ms"),
    ("security_rijndael_encode", "c4", 1.2612, 1.0184, "[0.981, 1.030]", "10/15", "0.45 ms"),
    ("automotive_susan_smoothing", "c1", 1.5422, 1.3680, "[1.365, 1.369]", "9/9", "27.6 ms"),
]

COLLECTOR_TMPL = r'''
import json, glob, os, re
out = {}
for f in sorted(glob.glob("%s/*.log")):
    tid = os.path.basename(f)[:-4]
    txt = open(f, errors="replace").read()
    rec = {"log_mtime": os.path.getmtime(f)}
    if tid.startswith(("po_", "oc_")):
        i = txt.find("\n{\n") if tid.startswith("po_") else txt.rfind("\n{\n")
        if i >= 0:
            try:
                d = json.loads(txt[i:])
                rec.update(final=d.get("confirmed_speedup"), median=d.get("confirmed_speedup"),
                           status=d.get("status"), explored=d.get("explored_best_speedup") or d.get("best_speedup"),
                           iqr=d.get("speedup_iqr"), base_ms=d.get("baseline_ms"),
                           npos=d.get("n_positive"), nruns=d.get("n"), err_msg=d.get("error"))
                if tid.startswith("po_"):
                    rec.update(noimp=d.get("no_improvement_over_O3"),
                               acc=len(re.findall(r"ACCEPTED", txt)),
                               rej=len(re.findall(r"REJECTED", txt)),
                               fail=len(re.findall(r"FAILED", txt)))
            except Exception as e:
                rec["err"] = str(e)[:80]
        else:
            rec["err"] = "no json block"
    else:
        m = re.findall(r"结果 JSON:\s*(\S+)", txt)
        if m and os.path.exists(m[-1]):
            try:
                d = json.load(open(m[-1]))
                rec.update(final=d.get("final_speedup"), median=d.get("confirmed_median"),
                           best_obs=d.get("best_observed_speedup"), status=d.get("final_status"),
                           explored=d.get("best_speedup"), base_ms=d.get("baseline_ms"),
                           npos=d.get("n_positive"), nruns=d.get("n_runs"),
                           iqr=(d.get("confirmation") or {}).get("speedup_iqr"),
                           steps=d.get("steps_taken"), rewrite=d.get("has_source_rewrite"),
                           nflags=len(d.get("best_flags") or []))
            except Exception as e:
                rec["err"] = str(e)[:80]
        else:
            rec["err"] = "no result json"
    out[tid] = rec
print(json.dumps(out))
'''


def ssh_py(host, script):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "python3 -"],
                       input=script, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        sys.exit(f"remote python failed on {host}: {r.stderr[:400]}")
    return json.loads(r.stdout.strip().splitlines()[-1])


def gm(v):
    return math.exp(sum(math.log(max(x, 1e-9)) for x in v) / len(v))


def load(state_path, logdir):
    state = ssh_py(QUEUE_HOST, "import json;print(json.dumps(json.load(open(%r))))" % state_path)
    tasks = state["tasks"] if isinstance(state, dict) else state
    data = {s: ssh_py(h, COLLECTOR_TMPL % logdir) for s, h in NODES.items()}
    for t in tasks:
        host = "-".join((t.get("node") or "").split("-")[:3])
        t["res"] = data.get(host, {}).get(t["id"])
    return tasks


def speedup(res):
    """Median paired speedup. Pre-2026-08-02 runs stored the MAX in
    final_speedup, so prefer the separately-stored median when present."""
    if not res:
        return None
    m = res.get("median")
    return float(m) if m is not None else res.get("final")


def significant(res):
    """Current rule: IQR entirely above 1.0 AND every paired run a gain."""
    if not res:
        return False
    iqr, npos, n = res.get("iqr"), res.get("npos"), res.get("nruns")
    if not iqr or npos is None or not n:
        return False
    return float(iqr[0]) > 1.0 and int(npos) == int(n)


def verdict(t):
    """('可用' | reason, is_clean)."""
    prog = t["program"].split("/")[-1][:-2]
    reasons = []
    if prog in TIER_CHANGED:
        reasons.append("正确性档位过松")
    lo_hi = ORPHAN.get("-".join((t.get("node") or "").split("-")[:3]))
    f = t.get("finished") or 0
    if lo_hi and lo_hi[0] < f < lo_hi[1]:
        reasons.append("孤儿抢核")
    if t["id"].startswith("po_") and PO_PREFIX_DISCARDED and f and f < ORPHAN["dgx-spark-a"][1]:
        reasons.append("PO 预算被 InstCombine 吞")
    return ("、".join(reasons) if reasons else "可用"), not reasons


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--archive", action="store_true", help="read the pre-fix archived sweep")
    ap.add_argument("--state"); ap.add_argument("--logdir"); ap.add_argument("-o", "--out")
    a = ap.parse_args()
    state_path = a.state or (ARCHIVE_STATE if a.archive else LIVE_STATE)
    logdir = a.logdir or (ARCHIVE_LOGDIR if a.archive else LIVE_LOGDIR)
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
    out = Path(a.out or f"docs/RESULTS_{today}.md")

    tasks = load(state_path, logdir)
    cond = lambda t: t["id"].split("_")[0]
    prog = lambda t: t["program"].split("/")[-1][:-2]

    def dur(t):
        if t.get("started") and t.get("finished"):
            m = (t["finished"] - t["started"]) / 60
            return f"{m:.0f}" if m >= 1 else f"{m*60:.0f}s"
        return "—"

    def rows(c, clean_only):
        best = {}
        for t in tasks:
            if cond(t) != c or t["status"] != "done":
                continue
            if speedup(t.get("res")) is None:
                continue
            if clean_only and not verdict(t)[1]:
                continue
            k = t["program"]
            if k not in best or (t.get("finished") or 0) > (best[k].get("finished") or 0):
                best[k] = t
        return sorted(best.values(), key=lambda t: t["id"])

    L = []
    W = L.append
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
    W("# COMET 消融实验 — 结果数据（按有效性分级）\n")
    W(f"_生成时间：{now} UTC，数据源 `{state_path}` + `{logdir}`_")
    W("_由 `scripts/gen_results_doc.py` 自动生成，请勿手工编辑_\n")

    W("## 0. 读这份文档之前必须知道的三件事\n")
    W("**(1) 这里的加速比是 n 次配对测量的中位数，不是最大值。** 2026-08-02 之前 "
      "`final_speedup` 存的是 n 次里的最大值。对带噪样本取最大值在 n=3 时按构造上偏约 "
      "0.85 个标准差：`telecom_crc32` 的三次比值是 0.853 / 1.485 / 1.635，发布值 1.6346x，"
      "而空闲核上独立配对复测是 1.016x。本文档一律从 `confirmed_median` 重算，"
      "新旧数据因此在同一把尺子上。\n")
    W("**(2) `显著` 也是重算的**：要求 IQR 整体位于 1.0 之上 **且** 每一次配对都为正。"
      "旧判据只要求中位数 >1.0，crc32 因此在 IQR = [0.853, 1.635]（连符号都没定下来）时"
      "被标成显著。\n")
    W("**(3) 每一行都带有效性判定。** 三类数据不可引用：\n")
    W("| 排除类别 | 含义 | 影响范围 |")
    W("|---|---|---|")
    W("| 正确性档位过松 | 该程序的判定 2026-08-02 从 `numeric`（1e-4 **相对**容差）收紧为 "
      "`hash`（逐字节）。crc32 打印 ~4e9 的校验和，1e-4 相对容差等于允许 ±40 万误差——"
      "凡是**采纳了改动**的结果都没被有效校验过 | 10 个程序 × 全部条件 |")
    W("| 孤儿抢核 | 两个 PPID=1 的遗留进程各霸占 pin 核 20+ 小时，与 worker 争抢同一个 CPU | "
      "2026-07-30 18:01 / 20:08 起至 07-31 17:30 之间完成的任务 |")
    W("| PO 预算被吞 | InstCombine 的 fixpoint 校验器 abort 掉 147 轮中的 19 轮，"
      "4 个程序三轮全废却记为 1.000x，整体偏向 1.0 | 修复前的全部 PO 任务 |")
    W("")

    # ── 1 进度
    W("## 1. 任务进度\n")
    cnt = Counter((cond(t), t["status"]) for t in tasks)
    W("| 条件 | done | running | pending | 合计 |")
    W("|---|---:|---:|---:|---:|")
    for c in ORDER:
        d, r, p = cnt[(c, "done")], cnt[(c, "running")], cnt[(c, "pending")]
        if d + r + p:
            W(f"| {SHORT[c]} | {d} | {r} | {p} | {d+r+p} |")
    tot = Counter(t["status"] for t in tasks)
    W(f"| **合计** | **{tot['done']}** | **{tot['running']}** | **{tot['pending']}** | **{len(tasks)}** |")
    W("")

    # ── 2 净集汇总（头条）
    W("## 2. 净集汇总（**这是可引用的数字**）\n")
    W("剔除上表三类数据后，每个 (条件, 程序) 取最近一次完成。\n")
    W("| 条件 | n | geomean | 中位数 | 最小 | 最大 | 显著数 |")
    W("|---|---:|---:|---:|---:|---:|---:|")
    for c in ORDER:
        g = rows(c, True)
        if not g:
            W(f"| {SHORT[c]} | 0 | — | — | — | — | — |")
            continue
        v = [speedup(t["res"]) for t in g]
        W(f"| {SHORT[c]} | {len(v)} | **{gm(v):.4f}** | {st.median(v):.4f} | {min(v):.4f} | "
          f"{max(v):.4f} | {sum(1 for t in g if significant(t['res']))} |")
    W("")
    W("### 2.1 净集分 suite\n")
    W("| 条件 | PolyBench n / geomean | cBench n / geomean |")
    W("|---|---|---|")
    for c in ORDER:
        g = rows(c, True)
        if not g:
            continue
        pb = [speedup(t["res"]) for t in g if "_pb" in t["id"]]
        cb = [speedup(t["res"]) for t in g if "_cb" in t["id"]]
        W(f"| {SHORT[c]} | {f'{len(pb)} / **{gm(pb):.4f}**' if pb else '—'} | "
          f"{f'{len(cb)} / **{gm(cb):.4f}**' if cb else '—'} |")
    W("")

    # ── 3 对照：旧口径 vs 新口径
    W("## 3. 口径影响（同一批数据，只换报告规则）\n")
    W("| 条件 | n | 旧口径 max-of-n | 新口径 median | 差异 |")
    W("|---|---:|---:|---:|---:|")
    for c in ORDER:
        g = [t for t in rows(c, True)
             if t["res"].get("final") is not None and t["res"].get("median") is not None]
        if not g:
            continue
        old = [t["res"]["final"] for t in g]
        new = [t["res"]["median"] for t in g]
        W(f"| {SHORT[c]} | {len(g)} | {gm(old):.4f} | **{gm(new):.4f}** | "
          f"{(gm(old)/gm(new)-1)*100:+.1f}% |")
    W("")

    # ── 4 独立复测
    W("## 4. 独立复测（空闲核，交替配对，输出逐字节比对）\n")
    W("对报告值最高的几个结果做的第三方验证。**正确性全部通过**——塌掉的是加速比，不是正确性。\n")
    W("| 程序 | 条件 | 报告值 | **独立复测** | 逐对 IQR | 正向 | 单次耗时 |")
    W("|---|---|---:|---:|---|---:|---:|")
    for p, c, rep, ver, iqr, pos, ms in VERIFIED:
        W(f"| {p} | {SHORT[c]} | {rep:.4f} | **{ver:.4f}** | {iqr} | {pos} | {ms} |")
    W("")
    W("规律很直接：**单次耗时低于 1 ms 的程序，报告的大幅加速全部塌回 1.0**——那个量级上"
      "测到的主要是进程启动开销，不是内核。唯一站住的是 susan_smoothing（27.6 ms），"
      "IQR 只有 ±0.2%、9/9 全正向。\n")

    # ── 5 逐任务明细
    W("## 5. 逐任务明细（含被排除的行）\n")
    for c in ORDER:
        g = []
        seen = {}
        for t in tasks:
            if cond(t) != c or t["status"] != "done" or speedup(t.get("res")) is None:
                continue
            k = t["program"]
            if k not in seen or (t.get("finished") or 0) > (seen[k].get("finished") or 0):
                seen[k] = t
        g = sorted(seen.values(), key=lambda t: t["id"])
        if not g:
            continue
        W(f"### {NAMES[c]}（{len(g)} 个）\n")
        W("| 任务 | 程序 | baseline (ms) | **中位加速比** | 旧口径(max) | IQR | n_pos/n | 显著 | "
          "探索期 best | 有效性 |")
        W("|---|---|---:|---:|---:|---|---:|:--:|---:|---|")
        for t in g:
            r = t["res"]
            iqr = r.get("iqr")
            v, ok = verdict(t)
            W(f"| `{t['id']}` | {prog(t)} | {r.get('base_ms') or 0:.2f} | "
              f"**{speedup(r):.4f}** | {(r.get('final') or 0):.4f} | "
              f"{f'[{iqr[0]:.3f}, {iqr[1]:.3f}]' if iqr else '—'} | "
              f"{r.get('npos','—')}/{r.get('nruns','—')} | {'✓' if significant(r) else ''} | "
              f"{(r.get('explored') or 0):.4f} | {'✓ 可用' if ok else '✗ ' + v} |")
        W("")

    # ── 6 正确性
    inc = [t for t in tasks if t.get("res") and t["res"].get("status") == "incorrect"]
    W("## 6. 正确性验证失败的任务\n")
    W(f"共 **{len(inc)}** 个。判定档位由 `src/correctness.py` 自动选择："
      "输出确定且含非整数值 → `numeric`；输出确定且全为整数值 → `hash`（逐字节）；"
      "参考程序自身不确定 → `exit_only`。\n")
    if inc:
        W("| 任务 | 程序 | 失败原因 |")
        W("|---|---|---|")
        for t in sorted(inc, key=lambda x: x["id"]):
            W(f"| `{t['id']}` | {prog(t)} | {(t['res'].get('err_msg') or '—')[:140]} |")
        W("")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L), encoding="utf-8")
    clean_total = sum(len(rows(c, True)) for c in ORDER)
    print(f"wrote {out}  ({len(L)} lines; {tot['done']} done, {clean_total} 净集行)")


if __name__ == "__main__":
    main()
