#!/usr/bin/env python3
"""Generate the full paper-data document: every task, every step, every effect.

    python3 scripts/gen_paper_data.py [-o docs/PAPER_DATA_<date>.md]

Unlike gen_results_doc.py (a summary), this dumps the complete experimental
record: for each task, the baseline, the auto-detected correctness tier, what
the agent did at EVERY step and what that step measured, why candidates were
rejected, and the final paired confirmation. It is meant to be read alongside
the paper, so a reviewer can trace any number back to the step that produced
it.

Three rules it enforces, each learned from a defect that silently produced
wrong numbers earlier in this study:

1. Resolve every task to the node the QUEUE says ran it. The same task id
   leaves a log on any node that ever ran it.
2. One row per (condition, program), most recently finished. Re-runs get new
   ids, so a cell can otherwise be counted twice.
3. Report the MEDIAN of the paired confirmation runs, recomputed from the
   stored confirmed_median. Data written before 2026-08-02 stored the MAX.
"""
import argparse, calendar, json, math, subprocess, sys, datetime
import statistics as st
from collections import Counter, defaultdict
from pathlib import Path

QUEUE_HOST = "oracle4"
LIVE_STATE = "/home/hanning/comet_queue/state.json"
ARCH_STATE = "/home/hanning/comet_queue/state_archive_20260802_prefix.json"
NODES = {"dgx-spark-a": "dss-dgx-a", "dgx-spark-b": "dss-dgx-b"}
LIVE_LOG = "/home/hanning/comet/logs_queue_run_v2"
ARCH_LOG = "/home/hanning/comet/logs_queue_run_v2_archive_20260802_prefix"

NAMES = {
    "c1": "① rewrite-only（每步强制 rewrite_source，屏蔽编译器反馈）",
    "c2": "② no-compiler-feedback（自由选动作，屏蔽编译器反馈）",
    "c3": "③ full system（自由选动作 + 完整编译器反馈）",
    "c4": "④ params-only（每步强制 try_flags）",
    "oc": "OC — OpenCode + DeepSeek 外部通用 agent baseline",
    "po": "PO — AutoPass (arXiv 2606.20373) 四-agent 复现 baseline",
}
ORDER = ["c1", "c2", "c3", "c4", "oc", "po"]
SHORT = {c: NAMES[c].split("（")[0].split(" —")[0] for c in ORDER}

# ── 有效性规则（每条都对应一个已定位并修复的缺陷）────────────────────────────
TIER_CHANGED = {"automotive_qsort1", "bzip2_decode", "network_dijkstra",
                "network_patricia", "office_stringsearch2", "telecom_crc32",
                "security_sha", "floyd-warshall", "nussinov", "correlation"}
CORRECTNESS_FIX_TS = calendar.timegm((2026, 8, 2, 17, 0, 0, 0, 0, 0))
PO_FIX_TS = calendar.timegm((2026, 8, 2, 17, 30, 0, 0, 0, 0))
ORPHAN = {"dgx-spark-a": (calendar.timegm((2026, 7, 30, 18, 1, 0, 0, 0, 0)),
                          calendar.timegm((2026, 7, 31, 17, 30, 0, 0, 0, 0))),
          "dgx-spark-b": (calendar.timegm((2026, 7, 30, 20, 8, 0, 0, 0, 0)),
                          calendar.timegm((2026, 7, 31, 17, 30, 0, 0, 0, 0)))}
# 变异测试（清空 kernel 函数体，门必须判失败）证明无法校验的程序
VACUOUS_GATE = {
    "heat-3d":   "dump 的数组与 kernel 完全无关，清空 kernel 后输出逐位相同（1e-12 精度下仍然如此）",
    "seidel-2d": "同上，任何打印精度下都无法检出 kernel 被清空",
    "automotive_qsort1": "输出仅 59 字节标题行，排序结果从不打印；把比较函数改成恒返回 0，输出逐字节相同",
    "consumer_tiff2median": "程序报 'Not a b&w image.' 直接退出，46 字节，从未执行计算",
}

# ── 编译器信息驱动的参数调整：独立复测 ────────────────────────────────────────
# 方法：把日志里 try_flags 步骤最终采纳的 -mllvm 选项原样取出，对原始源码单独
# 编译一次，在空闲核上与 -O3 基线做交替配对测量（9~11 组），并逐字节比对
# DUMP_ARRAYS 输出。这样测的是"仅靠编译器参数"的净效果，不含任何源码改写。
#
# 注意日志里的 `步骤N: X.XXXx` 是当时的**累计最优**而非该步增量。correlation 的
# `步骤2: 11.316x [rewrite]` 之后 `步骤3: 11.231x [flags]`，flags 那步其实是
# -0.085。按步直接归因会把源码重写的功劳算到参数头上。
# 选项字符串一律用正则从日志的 `步骤N: X.XXXx [flags: ...]` 整行提取，不从打印
# 输出转抄——floyd-warshall 就是这么错过一次：显示被截断到 88 字符，漏掉了
# `-mllvm -inline-threshold=225`，用不完整的选项复测出 0.9997，补齐后是 0.9885
# （结论未变，但数值曾是错的）。
FLAG_EXPLORE_CLAIMS = [   # 探索期读数（每步累计 - 此前最优 = 该步增量）
    ("consumer_tiff2rgba", "-vectorize-memory-check-threshold=256 -slp-max-vf=8", 6.542, 1.0112, "6/11"),
    ("telecom_crc32",      "-licm-max-num-uses-traversed=16",                     4.229, 0.9628, "1/11"),
    ("office_stringsearch2", "--unroll-max-upperbound=64",                        2.094, 0.9988, "5/11"),
    ("network_patricia",   "--licm-max-num-int-reassociations=32",                1.988, 1.0224, "9/11"),
    ("floyd-warshall",     "-unroll-threshold=1500 -vectorizer-min-trip-count=1 -inline-threshold=225", 2.640, 0.9885, "3/9"),
    ("jacobi-1d",          "-unroll-threshold=1000",                              1.594, 0.9942, "5/11"),
]
FLAG_CONFIRMED = [        # 条件 ④ 通过确认门的结果
    ("2mm",       "-partial-unrolling-threshold=30",                              1.2375, 1.1139, "8/9",  "[1.1106, 1.1239]"),
    ("jacobi-2d", "--partial-unrolling-threshold=256",                            1.1735, 1.0989, "9/9",  "[1.0982, 1.1005]"),
    ("adi",       "--partial-unrolling-threshold=32 --instcombine-guard-widening-window=64 --unroll-max-iteration-count-to-analyze=64", 1.0714, 1.0787, "8/9", "[1.0268, 1.1100]"),
    ("lu",        "-partial-unrolling-threshold=8000",                            1.1028, 1.0135, "8/9",  "[1.0065, 1.0163]"),
    ("durbin",    "-vectorize-memory-check-threshold=32",                         1.0537, 0.9925, "2/9",  "[0.9875, 0.9999]"),
    ("heat-3d",   "-partial-unrolling-threshold=4000 -slp-max-vf=4",              1.0623, 0.8708, "0/9",  "[0.8555, 0.8719]"),
]

COLLECTOR = r'''
import json, glob, os, re
LOGDIRS = {"live": %r, "arch": %r}
out = {}
for tag, ld in LOGDIRS.items():
    for f in sorted(glob.glob(ld + "/*.log")):
        tid = os.path.basename(f)[:-4]
        txt = open(f, errors="replace").read()
        rec = {"mtime": os.path.getmtime(f)}
        # 逐步记录：`步骤N: <加速比>x [动作: 描述]` 或 `步骤N: 失败 [动作] 原因`
        steps = []
        for m in re.finditer(r"^\s*步骤(\d+):\s*(.+)$", txt, re.M):
            steps.append({"step": int(m.group(1)), "text": m.group(2).strip()[:400]})
        rec["steps"] = steps
        mb = re.search(r"基线 -O3:\s*([\d.]+)\s*ms", txt)
        if mb: rec["log_base_ms"] = float(mb.group(1))
        mm = re.search(r"正确性验证模式:\s*(\w+)", txt)
        if mm: rec["mode"] = mm.group(1)
        mc = re.search(r"确认加速比:\s*([\d.]+)x\s*\(IQR \[([\d.]+), ([\d.]+)\], n=(\d+)", txt)
        if mc: rec["conf_line"] = {"med": float(mc.group(1)), "iqr": [float(mc.group(2)), float(mc.group(3))], "n": int(mc.group(4))}
        rec["rejects"] = [x[:200] for x in re.findall(r"步骤\d+:\s*失败[^\n]*", txt)][:20]
        # AutoPass 的逐轮记录：每轮一条候选 pass 顺序 + 参数，接受/拒绝/编译失败
        rounds = []
        for m in re.finditer(r"^\[round (\d+)/(\d+)\]\s*(.+)$", txt, re.M):
            rounds.append({"round": int(m.group(1)), "of": int(m.group(2)),
                           "text": m.group(3).strip()[:600]})
        if rounds: rec["rounds"] = rounds
        # OpenCode 的逐轮记录：每轮 agent 交互 + measure.sh 反馈
        oc = []
        for m in re.finditer(r"^\[\d\d:\d\d:\d\d\] round (\d+)/(\d+)[^\n]*$", txt, re.M):
            oc.append({"round": int(m.group(1)), "of": int(m.group(2))})
        if oc: rec["oc_rounds"] = len(oc)
        # 每步的动作与推理摘要（Action/Reasoning 行），与 步骤N 配对
        acts = re.findall(r"^\s*Action:\s*(\w+)\s*$", txt, re.M)
        if acts: rec["actions"] = acts[:20]
        if tid.startswith(("po_", "oc_")):
            i = txt.find("\n{\n") if tid.startswith("po_") else txt.rfind("\n{\n")
            if i >= 0:
                try:
                    d = json.loads(txt[i:])
                    rec.update(final=d.get("confirmed_speedup"), median=d.get("confirmed_speedup"),
                               status=d.get("status"), explored=d.get("explored_best_speedup") or d.get("best_speedup"),
                               iqr=d.get("speedup_iqr"), base_ms=d.get("baseline_ms"),
                               npos=d.get("n_positive"), nruns=d.get("n"), err_msg=d.get("error"),
                               prog=(d.get("program") or "").split("/")[-1].removesuffix(".c"))
                    if tid.startswith("po_"):
                        rec.update(noimp=d.get("no_improvement_over_O3"),
                                   passes=d.get("best_passes"), params=d.get("best_params"))
                except Exception as e: rec["err"] = str(e)[:80]
        else:
            m2 = re.findall(r"结果 JSON:\s*(\S+)", txt)
            if m2 and os.path.exists(m2[-1]):
                try:
                    d = json.load(open(m2[-1]))
                    conf = d.get("confirmation") or {}
                    rec.update(final=d.get("final_speedup"), median=d.get("confirmed_median"),
                               best_obs=d.get("best_observed_speedup"), status=d.get("final_status"),
                               explored=d.get("best_speedup"), base_ms=d.get("baseline_ms"),
                               npos=d.get("n_positive"), nruns=d.get("n_runs"),
                               iqr=conf.get("speedup_iqr"), cv_base=conf.get("base_stdev_pct"),
                               cv_best=conf.get("best_stdev_pct"),
                               steps_taken=d.get("steps_taken"), rewrite=d.get("has_source_rewrite"),
                               flags=d.get("best_flags"), feedback=d.get("feedback_used"),
                               prog=(d.get("program") or "").split("/")[-1].removesuffix(".c"))
                except Exception as e: rec["err"] = str(e)[:80]
        out[tag + "|" + tid] = rec
print(json.dumps(out))
''' % (LIVE_LOG, ARCH_LOG)


def ssh_py(host, script):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "python3 -"],
                       input=script, capture_output=True, text=True, timeout=900)
    if r.returncode:
        sys.exit(f"remote failed on {host}: {r.stderr[:300]}")
    return json.loads(r.stdout.strip().splitlines()[-1])


def gm(v):
    return math.exp(sum(math.log(max(x, 1e-9)) for x in v) / len(v))


def sign_test(pairs):
    w = sum(1 for a, b in pairs if b > a)
    l = sum(1 for a, b in pairs if b < a)
    n = w + l
    if not n:
        return w, l, 1.0
    p = sum(math.comb(n, i) for i in range(max(w, l), n + 1)) / 2 ** n * 2
    return w, l, min(p, 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
    out = Path(a.out or f"docs/PAPER_DATA_{today}.md")

    live = ssh_py(QUEUE_HOST, "import json;print(json.dumps(json.load(open(%r))))" % LIVE_STATE)["tasks"]
    arch = ssh_py(QUEUE_HOST, "import json;print(json.dumps(json.load(open(%r))))" % ARCH_STATE)["tasks"]
    logs = {s: ssh_py(h, COLLECTOR) for s, h in NODES.items()}

    def rec(tag, t):
        host = "-".join((t.get("node") or "").split("-")[:3])
        return logs.get(host, {}).get(f"{tag}|{t['id']}")

    def verdict(t, r, source):
        prog = r.get("prog") or t["program"].split("/")[-1].removesuffix(".c")
        f = t.get("finished") or r.get("mtime") or 0
        why = []
        if prog in VACUOUS_GATE:
            why.append("正确性门无效")
        if prog in TIER_CHANGED and f < CORRECTNESS_FIX_TS:
            why.append("正确性档位过松")
        lh = ORPHAN.get("-".join((t.get("node") or "").split("-")[:3]))
        if lh and lh[0] < f < lh[1]:
            why.append("孤儿抢核")
        if t["id"].startswith("po_") and f and f < PO_FIX_TS:
            why.append("PO 预算被 InstCombine 吞")
        return why

    def usable_speedup(r):
        """本次任务的加速比，取不到则 None。

        `baseline_only`（预算跑完但没有任何改动通过确认）和 `incorrect`
        （产物未通过正确性比对）都没有 confirmed_median，但它们是**真实的数据
        点**，含义是"这个方法在这个程序上没拿到收益"，按定义 1.0。早先版本只
        认 confirmed_median，于是把这些任务整个丢掉——等于只统计成功案例，
        系统性抬高每个条件的 geomean。条件 ① 在 cBench 上 19 个程序只剩 10 个
        进入统计，丢掉的 9 个全是 baseline_only。
        """
        if r.get("median") is not None:
            return r["median"]
        if r.get("status") in ("baseline_only", "incorrect"):
            return 1.0
        return None

    # 汇总：每 (条件, 程序) 取最近一次完成，live 优先，archive 补空
    cells = {}
    for tag, tasks in (("live", live), ("arch", arch)):
        for t in tasks:
            if t["status"] != "done":
                continue
            r = rec(tag, t)
            if not r:
                continue
            sp = usable_speedup(r)
            if sp is None:
                continue
            r = dict(r, median=sp)
            prog = r.get("prog") or t["program"].split("/")[-1].removesuffix(".c")
            key = (t["id"].split("_")[0], prog)
            fin = t.get("finished") or r.get("mtime") or 0
            if key in cells and cells[key][3] >= fin:
                continue
            cells[key] = (t, r, tag, fin, verdict(t, r, tag))

    L = []
    W = L.append
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
    W("# COMET 论文数据全集 — 每任务 / 每步骤 / 每效果\n")
    W(f"_生成时间：{now} UTC，由 `scripts/gen_paper_data.py` 自动生成_\n")
    W("本文件是完整实验记录：每个任务的基线、自动判定的正确性档位、agent 每一步做了什么、"
      "该步实测多少、被拒候选及原因、以及最终配对确认。论文里任何一个数字都可以在这里回溯到"
      "产生它的那一步。\n")

    # ── 有效性
    W("## 1. 有效性保证\n")
    W("### 1.1 正确性门的变异测试\n")
    W("方法：把 kernel 函数体替换为 `return;`（等于什么都不算），重新编译并送入正确性检查。"
      "**门必须判失败**；判通过说明该 benchmark 无法校验任何计算错误。全部 30 个 PolyBench "
      "程序逐个测试。\n")
    W("| 打印精度 | 门有效 | 门无效 |")
    W("|---|---:|---|")
    W("| `%0.2lf`（原始，量化步长 1e-2） | 29/30 | heat-3d |")
    W("| `%0.2lf` + 量化容差 | 27/30 | heat-3d、**jacobi-1d**、**seidel-2d**（回归） |")
    W("| **`%0.12lf`（现用，量化步长 1e-12）** | **28/30** | heat-3d、seidel-2d |")
    W("")
    W("结论：提高打印精度同时解决了两个方向的问题——向量化重结合（~1e-13 相对）不再被误判，"
      "而清空 kernel 仍被检出。仅放宽容差则会让 jacobi-1d、seidel-2d 的门失效，是错误的取舍。\n")
    W("### 1.2 必须排除的程序\n")
    W("| 程序 | 原因 |")
    W("|---|---|")
    for p, why in sorted(VACUOUS_GATE.items()):
        W(f"| `{p}` | {why} |")
    W("")
    W("### 1.3 数据排除规则（按时间界定，修复后重跑的数据不受影响）\n")
    W("| 类别 | 含义 |")
    W("|---|---|")
    W("| 正确性档位过松 | 该程序判定 2026-08-02 从 `numeric`（1e-4 **相对**容差）收紧为 `hash`（逐字节）。"
      "telecom_crc32 打印 ~4e9 校验和，1e-4 相对容差允许 ±40 万误差 |")
    W("| 孤儿抢核 | 两个 PPID=1 的遗留进程各霸占 pin 核 20+ 小时，与 worker 争抢同一 CPU |")
    W("| PO 预算被吞 | InstCombine fixpoint 校验器 abort 掉 147 轮中的 19 轮，4 个程序三轮全废却记为 1.000x |")
    W("")

    # ── 总体
    W("## 2. 总体结果\n")
    W("加速比 = n 次交替配对测量比值的**中位数**（不是最大值）。"
      "`显著` 要求 IQR 整体位于 1.0 之上且每次配对均为正。\n")
    for suite, tag in (("PolyBench", "_pb"), ("cBench", "_cb")):
        rows = []
        for c in ORDER:
            vals = [(p, r) for (cc, p), (t, r, _, _, why) in cells.items()
                    if cc == c and tag in t["id"] and not why]
            if vals:
                rows.append((c, [r["median"] for _, r in vals], vals))
        if not rows:
            continue
        W(f"### 2.{1 if suite=='PolyBench' else 2} {suite}（已剔除无效数据）\n")
        W("| 条件 | n | geomean | 中位数 | 最小 | 最大 |")
        W("|---|---:|---:|---:|---:|---:|")
        for c, v, _ in rows:
            W(f"| {SHORT[c]} | {len(v)} | **{gm(v):.4f}** | {st.median(v):.4f} | {min(v):.4f} | {max(v):.4f} |")
        W("")
        # 配对检验
        byc = {c: {p: r["median"] for p, r in vals} for c, _, vals in rows}
        W("**配对符号检验**（同一程序两条件都有数据）：\n")
        W("| 对比 | n | 胜 | 负 | 前者 geomean | 后者 geomean | p | 结论 |")
        W("|---|---:|---:|---:|---:|---:|---:|---|")
        for x, y in (("c3", "c2"), ("c3", "c1"), ("c1", "c2"), ("c3", "c4"), ("c3", "po"), ("c3", "oc")):
            if x not in byc or y not in byc:
                continue
            common = sorted(set(byc[x]) & set(byc[y]))
            if not common:
                continue
            pr = [(byc[y][p], byc[x][p]) for p in common]
            w, l, pv = sign_test(pr)
            W(f"| {SHORT[x]} vs {SHORT[y]} | {len(common)} | {w} | {l} | "
              f"{gm([byc[x][p] for p in common]):.4f} | {gm([byc[y][p] for p in common]):.4f} | "
              f"{pv:.4f} | {'**显著**' if pv < 0.05 else '不显著'} |")
        W("")

    # ── 编译器信息的实际作用
    W("## 2.3 编译器信息驱动的参数调整，实际带来多少\n")
    W("这一节单独回答\"编译器反馈起了什么作用\"。做法是把 `try_flags` 步骤最终采纳的 "
      "`-mllvm` 选项原样取出，**只对原始源码**单独编译，在空闲核上与 -O3 基线交替配对"
      "测量，并逐字节比对 DUMP_ARRAYS 输出。测的是纯参数的净效果，不含任何源码改写。\n")
    W("> **一个必须避开的陷阱**：日志里的 `步骤N: X.XXXx` 是**当时的累计最优**，"
      "不是该步增量。correlation 的 `步骤2: 11.316x [rewrite]` 之后 "
      "`步骤3: 11.231x [flags]`——flags 那步实际是 **−0.085**。按步直接归因会把源码"
      "重写的功劳算到参数头上。下表的\"增量\"一律用 `该步累计 − 此前最优` 计算。\n")
    W("### 探索期看起来 >1.5x 的参数，全部不成立\n")
    W("| 程序 | 采纳的 -mllvm 选项 | 探索期读数 | **独立复测** | 正向 |")
    W("|---|---|---:|---:|---:|")
    for p, fl, claim, real, pos in FLAG_EXPLORE_CLAIMS:
        W(f"| {p} | `{fl}` | {claim:.3f} | **{real:.4f}** | {pos} |")
    W("")
    W("六个全部塌回 1.0 附近，输出逐字节一致——**塌掉的是加速比，不是正确性**。"
      "这些读数产生于探索期的单次测量，没有经过配对确认门。\n")
    W("### 通过确认门的参数结果：真实但幅度有限\n")
    W("| 程序 | 采纳的 -mllvm 选项 | 报告确认值 | **独立复测** | 正向 | 复测 IQR |")
    W("|---|---|---:|---:|---:|---|")
    for p, fl, claim, real, pos, iqr in FLAG_CONFIRMED:
        W(f"| {p} | `{fl}` | {claim:.4f} | **{real:.4f}** | {pos} | {iqr} |")
    W("")
    W("**结论：编译器参数调整确实有真实收益，但天花板很低。**"
      "经独立复测站得住的最大值是 2mm 的 **1.1139x**（`-mllvm -partial-unrolling-threshold=30`，"
      "8/9 正向，IQR 宽度仅 1.2%），其次 jacobi-2d 1.0989x（9/9，IQR ±0.1%）与 adi 1.0787x。"
      "**整个语料里没有任何一个经验证的 >1.5x 纯参数案例。**\n")
    W("两个不成立的：`durbin` 基线只有 2.2 ms，落在测不出来的区间；"
      "`heat-3d` 复测为 0.8708（0/9 正向），而它本来就因正确性门无效被排除。\n")
    W("与条件 ④ 的整体结果一致：PolyBench 上 ④ 的 geomean 是 1.029，"
      "确认值上限 1.2375。参数通道的贡献是**个位数百分比**，而源码重写通道是 2.2–2.5 倍。\n")

    # ── 逐任务逐步
    W("## 3. 逐任务逐步明细\n")
    W("每步格式：`步骤N: <该步实测加速比>x [<动作>: <具体做了什么>]`，"
      "失败步记录被拒原因。这是 agent 的完整决策轨迹。\n")
    for c in ORDER:
        items = sorted([(p, v) for (cc, p), v in cells.items() if cc == c])
        if not items:
            continue
        W(f"### {NAMES[c]}（{len(items)} 个程序）\n")
        for prog, (t, r, tag, fin, why) in items:
            iqr = r.get("iqr")
            W(f"<details><summary><b>{prog}</b> — 中位加速比 <b>{r['median']:.4f}x</b>"
              f"（基线 {r.get('base_ms') or 0:.2f} ms，{r.get('mode','?')} 校验，"
              f"{r.get('npos','?')}/{r.get('nruns','?')} 次为正"
              f"{'，⚠ ' + '、'.join(why) if why else ''}）</summary>\n")
            W(f"- 任务 `{t['id']}`，节点 `{t.get('node')}`，数据源 `{tag}`")
            W(f"- 探索期最好单次：{r.get('explored') or 0:.4f}x　最终确认："
              f"**{r['median']:.4f}x**"
              + (f"　IQR [{iqr[0]:.4f}, {iqr[1]:.4f}]" if iqr else "")
              + (f"　base_cv={r['cv_base']:.1f}% best_cv={r['cv_best']:.1f}%" if r.get("cv_base") is not None else ""))
            if r.get("flags"):
                W(f"- 最终采纳编译选项：`{' '.join(r['flags'])}`")
            if r.get("rewrite"):
                W("- 最终采纳了源码重写")
            if r.get("passes"):
                W(f"- 最终 pass 顺序（{len(r['passes'])} 个）：`{','.join(r['passes'])}`")
            if r.get("params"):
                W(f"- 最终 pass 参数：`{' '.join(r['params'])}`")
            if r.get("feedback"):
                W(f"- 实际获得的反馈通道：`{r['feedback']}`")
            if r.get("steps"):
                acts = r.get("actions") or []
                W("\n| 步 | 动作 | 该步实测 / 结果 |")
                W("|---:|---|---|")
                for s in r["steps"]:
                    act = acts[s["step"] - 1] if len(acts) >= s["step"] else ""
                    W(f"| {s['step']} | {act} | {s['text'].replace('|', '/')} |")
            elif r.get("rounds"):
                W("\n每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，"
                  "Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：\n")
                W("| 轮 | 结果 |")
                W("|---:|---|")
                for s in r["rounds"]:
                    W(f"| {s['round']}/{s['of']} | {s['text'].replace('|', '/')} |")
            elif r.get("oc_rounds"):
                W(f"\n外部 agent 共交互 {r['oc_rounds']} 轮；该 harness 只在最终确认阶段"
                  f"记录结果，逐轮原始输出见节点上的 `opencode_runs/{t['id']}/round_*.jsonl`。")
            else:
                W("\n（该 harness 不产生逐步记录）")
            W("\n</details>\n")

    # ── 失败分析
    W("## 4. 被拒候选分析\n")
    W("agent 提出但未被采纳的候选，按拒绝原因归类。这些不是 bug，是方法本身的一部分："
      "确认门拦掉了探索期的虚高读数与不正确的改写。\n")
    cnt = Counter()
    examples = defaultdict(list)
    for (c, p), (t, r, tag, fin, why) in cells.items():
        for line in r.get("rejects", []):
            if "Numeric mismatch" in line: k = "数值不符（多为浮点重结合）"
            elif "hash mismatch" in line: k = "哈希不符"
            elif "未找到匹配" in line: k = "pragma 未匹配到循环"
            elif "编译" in line or "error" in line: k = "编译失败"
            else: k = "其它"
            cnt[k] += 1
            if len(examples[k]) < 4:
                examples[k].append(f"`{c}` {p}: {line[:150]}")
    W("| 拒绝原因 | 次数 |")
    W("|---|---:|")
    for k, v in cnt.most_common():
        W(f"| {k} | {v} |")
    W("")
    for k, ex in examples.items():
        W(f"**{k}** 样例：\n")
        for e in ex:
            W(f"- {e}")
        W("")

    W("## 5. 数据来源与复算\n")
    W(f"- 队列：`{QUEUE_HOST}:{LIVE_STATE}`（当前）+ `{ARCH_STATE}`（2026-08-02 之前）")
    W(f"- 日志：各节点 `{LIVE_LOG}` 与 `{ARCH_LOG}`，按队列记录的 `node` 字段定位")
    W("- 加速比取 `confirmed_median`；2026-08-02 之前的数据 `final_speedup` 存的是 max-of-n，本文件不使用该字段")
    W("- 重跑任务使用新 id，每 (条件, 程序) 只取最近一次完成\n")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {out}  ({len(L)} lines, {len(cells)} 个 (条件,程序) 单元)")


if __name__ == "__main__":
    main()
