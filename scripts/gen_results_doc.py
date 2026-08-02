#!/usr/bin/env python3
"""Regenerate docs/RESULTS_FULL_<date>.md from live queue + node state.

Run it any time; it always reflects whatever has finished so far, so it can be
re-run mid-sweep for a progress snapshot and again at the end for the final
table. Nothing is read from a previous version of the document -- every number
is re-derived, so the doc can never drift from the data.

    python3 scripts/gen_results_doc.py                 # write docs/RESULTS_FULL_<today>.md
    python3 scripts/gen_results_doc.py -o path.md      # write somewhere else

THE ONE RULE THAT MATTERS: resolve every task to the node the QUEUE says ran
it. The same task id leaves a log on any node that ever ran it, and re-runs
routinely land on the other machine, so scanning logs directly will silently
mix invalidated results from a superseded sweep into the totals. `state.json`'s
`node` field is the only authority for which log is the live one.
"""
import argparse, json, math, subprocess, sys, datetime
import statistics as st
from collections import Counter
from pathlib import Path

QUEUE_HOST = "oracle4"
QUEUE_STATE = "/home/hanning/comet_queue/state.json"
NODES = {"dgx-spark-a": "dss-dgx-a", "dgx-spark-b": "dss-dgx-b"}
COMET = "/home/hanning/comet"

NAMES = {
    "c1": "① rewrite-only（禁用编译器反馈，每步强制 rewrite_source）",
    "c2": "② no-compiler-feedback（自由选动作，屏蔽编译器反馈）",
    "c3": "③ full system（自由选动作 + 完整编译器反馈）",
    "c4": "④ params-only（每步强制 try_flags）",
    "oc": "OC — OpenCode + DeepSeek 外部 CLI agent baseline",
    "po": "PO — AutoPass (arXiv 2606.20373) 四-agent 复现 baseline",
}
ORDER = ["c1", "c2", "c3", "c4", "oc", "po"]

# Runs on each measurement node; prints {task_id: {...}} as JSON.
COLLECTOR = r'''
import json, glob, os, re
out = {}
for f in sorted(glob.glob("%s/logs_queue_run_v2/*.log")):
    tid = os.path.basename(f)[:-4]
    txt = open(f, errors="replace").read()
    rec = {"log_mtime": os.path.getmtime(f)}
    if tid.startswith(("po_", "oc_")):
        # PO writes its JSON block first, OC last.
        i = txt.find("\n{\n") if tid.startswith("po_") else txt.rfind("\n{\n")
        if i >= 0:
            try:
                d = json.loads(txt[i:])
                rec.update(final=d.get("confirmed_speedup"), status=d.get("status"),
                           explored=d.get("explored_best_speedup") or d.get("best_speedup"),
                           sig=d.get("significant"), iqr=d.get("speedup_iqr"),
                           base_ms=d.get("baseline_ms"), npos=d.get("n_positive"),
                           err_msg=d.get("error"))
                if tid.startswith("po_"):
                    rec.update(noimp=d.get("no_improvement_over_O3"),
                               n_passes=len(d.get("best_passes") or []),
                               n_params=len(d.get("best_params") or []),
                               target=(d.get("score_agent_target") or {}).get("name"),
                               target_score=(d.get("score_agent_target") or {}).get("score"),
                               acc=len(re.findall(r"ACCEPTED", txt)),
                               rej=len(re.findall(r"REJECTED", txt)),
                               fail=len(re.findall(r"FAILED", txt)),
                               preflight=len(re.findall(r"pre-flight dropped", txt)))
            except Exception as e:
                rec["err"] = str(e)[:80]
        else:
            rec["err"] = "no json block"
    else:
        m = re.findall(r"结果 JSON:\s*(\S+)", txt)
        if m and os.path.exists(m[-1]):
            try:
                d = json.load(open(m[-1]))
                rec.update(final=d.get("final_speedup"), status=d.get("final_status"),
                           explored=d.get("best_speedup"), sig=d.get("significant_gain"),
                           base_ms=d.get("baseline_ms"), npos=d.get("n_positive"),
                           steps=d.get("steps_taken"), rewrite=d.get("has_source_rewrite"),
                           nflags=len(d.get("best_flags") or []),
                           rb_flags=d.get("rolled_back_flags"), rb_src=d.get("rolled_back_source"))
            except Exception as e:
                rec["err"] = str(e)[:80]
        else:
            rec["err"] = "no result json"
    out[tid] = rec
print(json.dumps(out))
''' % COMET


def ssh(host, script):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "python3 -"],
                       input=script, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        sys.exit(f"remote python failed on {host}: {r.stderr[:400]}")
    return json.loads(r.stdout.strip().splitlines()[-1])


def gm(v):
    return math.exp(sum(math.log(max(x, 1e-9)) for x in v) / len(v))


def load():
    state = ssh(QUEUE_HOST, "import json;print(json.dumps(json.load(open(%r))))" % QUEUE_STATE)
    tasks = state["tasks"] if isinstance(state, dict) else state
    data = {short: ssh(host, COLLECTOR) for short, host in NODES.items()}
    for t in tasks:
        node = t.get("node") or ""
        host = "-".join(node.split("-")[:3])
        t["res"] = data.get(host, {}).get(t["id"])
    return tasks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out")
    args = ap.parse_args()
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
    out = Path(args.out or f"docs/RESULTS_FULL_{today}.md")

    tasks = load()
    cond = lambda t: t["id"].split("_")[0]
    prog = lambda t: t["program"].split("/")[-1][:-2]
    utc = lambda ts: (datetime.datetime.fromtimestamp(ts, datetime.timezone.utc)
                      .strftime("%m-%d %H:%M") if ts else "—")

    def dur(t):
        if t.get("started") and t.get("finished"):
            m = (t["finished"] - t["started"]) / 60
            return f"{m:.0f}" if m >= 1 else f"{m*60:.0f}s"
        return "—"

    def done(c):
        """Finished tasks for condition `c`, ONE per program.

        A re-run is enqueued under a new id (`po_cb005_r2`) so its log doesn't
        clobber the superseded one, which means the same (condition, program)
        cell can appear more than once. Counting both would double-weight that
        program in the geomean and mix a result from a fixed harness with the
        one it was meant to replace, so keep only the most recently finished
        run of each cell.
        """
        best = {}
        for t in tasks:
            if cond(t) != c or t["status"] != "done":
                continue
            if not t["res"] or t["res"].get("final") is None:
                continue
            key = t["program"]
            prev = best.get(key)
            if prev is None or (t.get("finished") or 0) > (prev.get("finished") or 0):
                best[key] = t
        return sorted(best.values(), key=lambda t: t["id"])

    L = []
    W = L.append
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
    W("# COMET 消融实验 — 全量精细数据快照\n")
    W(f"_生成时间：{now} UTC（由 `scripts/gen_results_doc.py` 自动生成，请勿手工编辑）_\n")
    W("每个数字都从队列 `state.json` 记录的任务→节点归属出发，回到该节点上对应的结果 JSON 重新提取。"
      "**同一个 task id 在多个节点上都可能留有日志**（重跑常落到另一台机器），"
      "只有 `state.json` 的 `node` 字段能决定哪一份是有效的。\n")

    # 1 环境
    W("## 1. 测量环境\n")
    W("| 项 | 值 |")
    W("|---|---|")
    W("| 架构 | aarch64，NVIDIA DGX Spark (GB10 Grace)，Cortex-X925 + Cortex-A725，20 核 |")
    W("| 节点 | `dgx-spark-a`、`dgx-spark-b`（各 1 个 worker，均 `--pin-cpu 2`） |")
    W("| 工具链 | Ubuntu clang 21.1.8 / opt-21 / llc-21，`aarch64-unknown-linux-gnu` |")
    W("| baseline | `clang -O3`，六个条件共用同一条编译路径 |")
    W("| 预算 | ①②③④/OC：9 步；PO：3 轮（R3，对齐论文主结果配置） |")
    W("| 最终确认 | 与 -O3 交替配对测量，`runs=3` |")
    W("")

    # 2 进度
    W("## 2. 总体进度\n")
    cnt = Counter((cond(t), t["status"]) for t in tasks)
    W("| 条件 | done | running | pending | 合计 |")
    W("|---|---:|---:|---:|---:|")
    for c in ORDER:
        d, r, p = cnt[(c, "done")], cnt[(c, "running")], cnt[(c, "pending")]
        if d + r + p:
            W(f"| {NAMES[c].split('（')[0].split(' —')[0]} | {d} | {r} | {p} | {d+r+p} |")
    tot = Counter(t["status"] for t in tasks)
    W(f"| **合计** | **{tot['done']}** | **{tot['running']}** | **{tot['pending']}** | **{len(tasks)}** |")
    W("")

    # 3 汇总
    W("## 3. 六条件汇总\n")
    W("| 条件 | n | geomean | 中位数 | 最小 | 最大 | >1.05 | 恰好 1.000 | <0.95 |")
    W("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for c in ORDER:
        g = done(c)
        if not g:
            continue
        v = [t["res"]["final"] for t in g]
        W(f"| {NAMES[c].split('（')[0].split(' —')[0]} | {len(v)} | **{gm(v):.4f}** | {st.median(v):.4f} | "
          f"{min(v):.4f} | {max(v):.4f} | {sum(1 for x in v if x>1.05)} | "
          f"{sum(1 for x in v if abs(x-1)<1e-6)} | {sum(1 for x in v if x<0.95)} |")
    W("")
    W("> `incorrect` = 产物未通过与 -O3 参考输出的比对，该任务加速比被强制置为 1.0000 后计入 geomean。\n")
    W("| 条件 | confirmed | baseline_only | incorrect | 其它 |")
    W("|---|---:|---:|---:|---|")
    for c in ORDER:
        g = done(c)
        if not g:
            continue
        sc = Counter(t["res"].get("status") for t in g)
        other = {k: v for k, v in sc.items()
                 if k not in ("confirmed", "baseline_only", "incorrect")}
        W(f"| {NAMES[c].split('（')[0].split(' —')[0]} | {sc.get('confirmed',0)} | "
          f"{sc.get('baseline_only',0)} | {sc.get('incorrect',0)} | {other or '—'} |")
    W("")
    W("### 3.1 分 suite\n")
    W("| 条件 | PolyBench n / geomean | cBench n / geomean |")
    W("|---|---|---|")
    for c in ORDER:
        g = done(c)
        if not g:
            continue
        pb = [t["res"]["final"] for t in g if "_pb" in t["id"]]
        cb = [t["res"]["final"] for t in g if "_cb" in t["id"]]
        W(f"| {NAMES[c].split('（')[0].split(' —')[0]} | "
          f"{f'{len(pb)} / **{gm(pb):.4f}**' if pb else '—'} | "
          f"{f'{len(cb)} / **{gm(cb):.4f}**' if cb else '—'} |")
    W("")

    # 4 PO
    po = done("po")
    if po:
        W("## 4. PO — AutoPass 复现（逐任务）\n")
        W("`回退O3` = 三轮都没赢过 `default<O3>`，最终二进制就是 LLVM 自带 -O3 pipeline，"
          "**此时确认值理论上必须是 1.000**，偏离多少就是该子集的噪声底。\n")
        W("| 任务 | 程序 | baseline (ms) | 探索期 best | **确认值** | IQR | 显著 | 回退O3 | 接受/拒绝/失败 | #pass | #param | 节点 | 用时(min) |")
        W("|---|---|---:|---:|---:|---|:--:|:--:|:--:|---:|---:|---|---:|")
        for t in po:
            r = t["res"]
            iqr = r.get("iqr")
            W(f"| `{t['id']}` | {prog(t)} | {r.get('base_ms') or 0:.2f} | "
              f"{r.get('explored') or 0:.4f} | **{r['final']:.4f}** | "
              f"{f'[{iqr[0]:.3f}, {iqr[1]:.3f}]' if iqr else '—'} | "
              f"{'✓' if r.get('sig') else ''} | {'✓' if r.get('noimp') else ''} | "
              f"{r.get('acc','—')}/{r.get('rej','—')}/{r.get('fail','—')} | "
              f"{r.get('n_passes','—')} | {r.get('n_params','—')} | {t['node']} | {dur(t)} |")
        v = [t["res"]["final"] for t in po]
        ex = [t["res"]["explored"] for t in po if t["res"].get("explored")]
        fails = sum(t["res"].get("fail", 0) for t in po)
        W("")
        W(f"**搜索预算**：{len(po)}×3 = {len(po)*3} 次候选评估，"
          f"ACCEPTED {sum(t['res'].get('acc',0) for t in po)}／"
          f"REJECTED {sum(t['res'].get('rej',0) for t in po)}／"
          f"FAILED {fails}（{fails/max(1,len(po)*3)*100:.1f}%）；"
          f"至少接受过一轮的程序 {sum(1 for t in po if t['res'].get('acc',0)>0)}/{len(po)}。\n")
        ctrl = [t["res"]["final"] for t in po if t["res"].get("noimp")]
        if ctrl:
            cpb = [t["res"]["final"] for t in po if t["res"].get("noimp") and "_pb" in t["id"]]
            ccb = [t["res"]["final"] for t in po if t["res"].get("noimp") and "_cb" in t["id"]]
            W("**自校准控制组**（应恰好 1.000）：")
            for nm, sub in (("PolyBench", cpb), ("cBench", ccb)):
                if sub:
                    W(f"- {nm} n={len(sub)}，平均绝对偏差 {sum(abs(x-1) for x in sub)/len(sub)*100:.1f}%，"
                      f"区间 [{min(sub):.4f}, {max(sub):.4f}]")
            W("")
        if ex:
            pb = [t["res"]["final"] for t in po if "_pb" in t["id"]]
            cb = [t["res"]["final"] for t in po if "_cb" in t["id"]]
            expb = [t["res"]["explored"] for t in po if "_pb" in t["id"] and t["res"].get("explored")]
            excb = [t["res"]["explored"] for t in po if "_cb" in t["id"] and t["res"].get("explored")]
            W("### 4.1 与论文 Table 5 对齐\n")
            W("论文口径 = 三轮中观测到的最好值，**不做独立复测**（Table 5 表注 "
              "\"best performance in three refinement rounds\"）。本项目的确认值是重新编译后"
              "与 -O3 交替配对复跑 n=3 的结果。\n")
            W("| Suite | 本项目（确认口径） | 本项目（论文口径） | 论文 x86-64 | 论文 ARM64 |")
            W("|---|---:|---:|---:|---:|")
            if cb:
                W(f"| cBench | {gm(cb):.4f} (n={len(cb)}) | {gm(excb):.4f} | 1.059 | 1.111 |")
            if pb:
                W(f"| PolyBench | {gm(pb):.4f} (n={len(pb)}) | {gm(expb):.4f} | 1.009 | 1.149 |")
            W(f"| **合计** | **{gm(v):.4f}** (n={len(v)}) | **{gm(ex):.4f}** | **1.043** | **1.117** |")
            W(f"\n两个口径相差 {(gm(ex)/gm(v)-1)*100:.1f}%，即\"三次带噪测量取最大值\"的 selection bias。\n")

    # 5 comet 条件
    W("## 5. 条件 ①②③④（逐任务）\n")
    for c in ("c1", "c2", "c3", "c4"):
        g = done(c)
        if not g:
            continue
        W(f"### {NAMES[c]}（n={len(g)}）\n")
        W("| 任务 | 程序 | baseline (ms) | 步数 | 探索期 best | **确认值** | status | 显著 | 源码重写 | #flags | 回退 | 节点 | 用时(min) |")
        W("|---|---|---:|---:|---:|---:|---|:--:|:--:|---:|---|---|---:|")
        for t in g:
            r = t["res"]
            rb = "+".join([x for x, y in (("flags", r.get("rb_flags")), ("src", r.get("rb_src"))) if y])
            W(f"| `{t['id']}` | {prog(t)} | {r.get('base_ms') or 0:.2f} | {r.get('steps','—')} | "
              f"{r.get('explored') or 0:.4f} | **{r['final']:.4f}** | {r.get('status','—')} | "
              f"{'✓' if r.get('sig') else ''} | {'✓' if r.get('rewrite') else ''} | "
              f"{r.get('nflags','—')} | {rb} | {t['node']} | {dur(t)} |")
        v = [t["res"]["final"] for t in g]
        pb = [t["res"]["final"] for t in g if "_pb" in t["id"]]
        cb = [t["res"]["final"] for t in g if "_cb" in t["id"]]
        W("")
        W(f"**小结**：geomean **{gm(v):.4f}**"
          f"（PolyBench {gm(pb):.4f} n={len(pb)}；cBench {gm(cb):.4f} n={len(cb)}）"
          f"／中位数 {st.median(v):.4f}／区间 [{min(v):.4f}, {max(v):.4f}]"
          f"／`baseline_only` {sum(1 for t in g if t['res'].get('status')=='baseline_only')} 个"
          f"／发生源码重写 {sum(1 for t in g if t['res'].get('rewrite'))} 个。\n")

    # 6 OC
    g = done("oc")
    if g:
        W(f"## 6. {NAMES['oc']}（n={len(g)}）\n")
        W("| 任务 | 程序 | baseline (ms) | 探索期 best | **确认值** | status | 显著 | 节点 | 用时(min) |")
        W("|---|---|---:|---:|---:|---|:--:|---|---:|")
        for t in g:
            r = t["res"]
            W(f"| `{t['id']}` | {prog(t)} | {r.get('base_ms') or 0:.2f} | {r.get('explored') or 0:.4f} | "
              f"**{r['final']:.4f}** | {r.get('status','—')} | {'✓' if r.get('sig') else ''} | "
              f"{t['node']} | {dur(t)} |")
        v = [t["res"]["final"] for t in g]
        W("")
        W(f"**小结**：geomean **{gm(v):.4f}**／中位数 {st.median(v):.4f}／"
          f"区间 [{min(v):.4f}, {max(v):.4f}]。\n")

    # 7 正确性
    W("## 7. 正确性验证结果\n")
    inc = [(t, t["res"]) for t in tasks
           if t["res"] and t["res"].get("status") == "incorrect"]
    W(f"`incorrect` 任务共 **{len(inc)}** 个。判定档位由 `src/correctness.py` 自动选择："
      "输出确定且含非整数值 → `numeric`（相对容差，容忍浮点重结合）；"
      "输出确定且全为整数值 → `hash`（逐字节精确）；参考程序自身不确定 → `exit_only`。\n")
    if inc:
        W("| 任务 | 程序 | 失败原因 |")
        W("|---|---|---|")
        for t, r in sorted(inc, key=lambda x: x[0]["id"]):
            W(f"| `{t['id']}` | {prog(t)} | {(r.get('err_msg') or '—')[:150]} |")
        W("")

    # 8 数据质量
    W("## 8. 数据质量自检\n")
    W("### 8.1 探索期最好值 vs 最终确认值\n")
    W("比值落在 [0.7, 1.3] 之外的任务。偏低说明探索期读数虚高；"
      "**偏高没有物理解释**——最终 pipeline 不可能比搜索过程中测到的最好结果还快。\n")
    rows = []
    for t in tasks:
        r = t.get("res")
        if not r or r.get("final") is None:
            continue
        e, f = r.get("explored"), r["final"]
        if e and e > 0 and r.get("status") == "confirmed" and not (0.7 <= f / e <= 1.3):
            rows.append((t["id"], prog(t), e, f, f / e, r.get("base_ms")))
    if rows:
        W("| 任务 | 程序 | 探索期 best | 确认值 | 比值 | baseline (ms) |")
        W("|---|---|---:|---:|---:|---:|")
        for tid, p, e, f, ratio, b in sorted(rows, key=lambda x: -abs(math.log(x[4]))):
            W(f"| `{tid}` | {p} | {e:.3f} | {f:.3f} | {ratio:.2f} | {b or 0:.2f} |")
        W(f"\n共 {len(rows)} 个，其中 {sum(1 for r in rows if r[4] > 1.3)} 个是\"确认值反而更高\"。\n")
    else:
        W("无。\n")

    W("### 8.2 极端加速比\n")
    W("geomean 对单点异常极其敏感，>5x 的结果必须人工核对产物是否真的等价。\n")
    ext = [(t["id"], prog(t), t["res"]["final"]) for t in tasks
           if t.get("res") and (t["res"].get("final") or 0) > 5]
    if ext:
        W("| 任务 | 程序 | 确认值 |")
        W("|---|---|---:|")
        for tid, p, f in sorted(ext, key=lambda x: -x[2]):
            W(f"| `{tid}` | {p} | **{f:.4f}** |")
        W("")
    else:
        W("无。\n")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {out}  ({len(L)} lines, {tot['done']} done / {len(tasks)} tasks)")


if __name__ == "__main__":
    main()
