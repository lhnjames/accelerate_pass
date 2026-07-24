> ⚠️ **已作废（SUPERSEDED，2026-07-24）**：本文件的数字来自 **quick-check + rounds=3** 的浅搜索，该模式砍掉了 exhaustive flag 补充搜索，会低估参数通道的效果。已按用户要求删除对应远端运行数据，并以 **rounds=5、无 quick-check** 的完整深度消融重跑取代。最终数字见 r5 结果目录 (`cbench_r5_logs/`、`polybench_r5_logs/`) 与后续新报告。以下内容仅保留方法学与设计说明，数字不作数。

# PolyBench 编译器反馈消融 — 完整结果快照（2026-07-23）

> **这是一个进行中扫描的快照，不是最终结果。** 全 30 程序单 seed 扫描共 60 cell，
> 本快照时已完成 **10 个程序**（9 优先 kernel + gemm）的 full/no_feedback 配对。
> 所有 ≥ ~5x 的大加速**尚未做独立金标准正确性复核**，在复核通过前一律标注为
> 「pipeline-confirmed，待复核」，不作为最终定论。

---

## 1. 这个实验在验证什么

论文核心问题：**LLVM/Clang 21 的编译器反馈（pass remarks、missed transformation、
IR/pass graph、perf 硬件热点）到底能不能帮 LLM 找到更好的优化？**

用两个**除反馈外完全匹配**的条件对比：

| 条件 | LLM 能看到什么 |
|---|---|
| **full** | 源码 + 编译命令 + 基线时间 + **pass remarks + missed transformation + IR/pass graph + perf(IPC/cache/branch) + pass 审计 + 热点重定向** + 自身历史 |
| **no_feedback** | 只有源码 + 编译器版本 + -O3 命令 + 基线时间 + 正确性契约 + 自身历史。**上面所有编译器/硬件反馈被完全屏蔽** |

屏蔽由代码强制（`_strip_compiler_feedback()`），并由标记串泄漏测试
（`tests/test_ablation_no_feedback.py`）保证：反馈标记**必须**出现在 full 的 prompt、
**一个都不能**出现在 no_feedback 的 prompt。

---

## 2. 扫描配置

| 项 | 值 |
|---|---|
| 主机 | `ubuntu@132.145.22.86`，4 核 aarch64 Neoverse-N1 |
| 编译器 | `/usr/bin/clang-21`（21.1.8） |
| 程序 | 全部 30 个 PolyBench/C 4.2 kernel（本快照完成 10 个） |
| 数据集 | `LARGE_DATASET` |
| 条件 | full, no_feedback |
| seed | 1（单 seed；用户决定只做条件对比，不做多 seed 稳定性） |
| 每 cell 预算 | `--rounds 3 --quick-check` |
| 确认测量 | `--runs 5`，baseline/candidate **交替** + 配对比值中位数 + 外部墙钟 |
| 绑核 / 并发 | `--pin-cpu 2` / 串行（避免带宽争抢污染计时） |
| 加速比定义 | `baseline_median / candidate_median` |

**上报纪律**：只有「确认过且 ≥ 1.0」才记非 1.0 的数字。确认 < 1.0（回归）或确认
未跑成 → 一律回滚到纯 -O3、记 1.0000x。探索期单次峰值只存档、不进正式表。

---

## 3. 逐 cell 结果（已完成 10 程序）

`final` = 正式确认加速比（n=5 交替）。`IQR>1` = 确认 IQR 是否**整段**高于 1.0
（判断微小增益是否真实的最严标准）。

| program | full | full IQR>1 | no_feedback | nf IQR>1 | 配对方向 |
|---|---:|:--:|---:|:--:|---|
| gramschmidt | **21.38x** | ✅ | **19.68x** | ✅ | ≈（full 略高） |
| covariance | **17.18x** | ✅ | **17.30x** | ✅ | ≈ |
| correlation | **17.30x** | ✅ | **16.97x** | ✅ | ≈ |
| 3mm | **9.96x** | ✅ | **10.38x** | ✅ | ≈（nf 略高） |
| floyd-warshall | 1.00（baseline）| — | **1.43x** | ✅ | **nf 明显更好** |
| nussinov | 1.10x | ✅ | 1.10x | ✅ | 平 |
| seidel-2d | 1.023x | ✅ | 1.019x | ✅ | ≈ |
| cholesky | 1.011x | ✅ | **timeout** | — | full 有值 / nf 超 2h 预算 |
| adi | 1.009x | ✅ | 1.00（baseline）| — | 本质都 ≈1.0 |
| gemm | 1.001x | ❌（噪声内）| 1.00（baseline）| — | 都≈1.0，已高度优化 |

说明：
- **gemm/full 1.001x 的 IQR = [0.999, 1.002] 跨越 1.0** → 判为噪声内、**非真增益**。
  这正是 IQR 判据该拦下的边缘情况。
- 4 个大加速（gramschmidt/covariance/correlation/3mm）的 IQR 都很窄且远高于 1.0，
  测量本身很稳——但**测量稳 ≠ 结果正确**，仍须金标准复核。
- 3mm 另有 seed-2 复现（full 9.69x / nf 10.76x），方向与 seed-1 一致。

---

## 4. 汇总统计（10 程序快照）

| 指标 | full | no_feedback |
|---|---:|---:|
| 可用 cell | 11（3mm 含 seed2） | 10 |
| 有结果的程序 | 10 | 9（cholesky 超时） |
| geomean（程序中位数） | 3.06x | 3.59x |
| 成功率 ≥ 1.0 | 100% | 100% |
| 严格增益 > 1.01 | 73% | 80% |
| IQR 支撑的真增益率 | 82% | 80% |
| baseline_only（未找到候选） | 1 | 2 |
| timeout | 0 | 1 |

**配对比较（10 对两条件都完成的 cell）**：
- geomean full = **3.84x**，no_feedback = **4.00x**
- 配对比值 full/no_feedback = **0.9614**，bootstrap **CI95 = [0.947, 1.011]**
- 逐条 head-to-head：**full 赢 6，no_feedback 赢 4**

---

## 5. 目前能说 / 不能说什么

### 能说（初步方向，非最终结论）

1. **no_feedback 没有系统性差于 full。** 配对比值 CI95 = [0.947, 1.011] **跨越 1.0**
   → 在已完成的 10 程序、seed 1 上，两个条件**没有统计显著差异**。
2. 一个反复出现的形态：**full 逐条赢的程序更多（6:4），但 no_feedback 赢的时候赢得更大**
   （floyd-warshall：nf 1.43x vs full 完全没找到候选）。两种效应抵消。
3. 若该方向在剩余 20 程序上延续，论文结论会是一个**反直觉但有价值**的发现：
   *对 PolyBench 这类结构规整的数值 kernel，LLM 主要靠源码本身就能找到大的优化
   （循环交换、tiling、访存重构）；LLVM 的 pass/perf 反馈边际收益很小，个别情况
   （floyd-warshall）甚至可能把它带偏。*

### 不能说（硬边界）

1. **大加速未独立验真**：gramschmidt ~20x、covariance/correlation ~17x、3mm ~10x
   目前只过了流水线自己的正确性门。这些主导了 geomean，**必须先过金标准逐位复核**。
   若任一复核 FAIL，对应加速比作废、整个 geomean 会大幅改变。
2. **单 seed**：无跨随机种子的稳定性证据。CI 反映的是**跨程序**离散度，不是跨 seed。
3. **只完成 10/30 程序**：剩 20 个（含大量小 BLAS kernel，预计多为 ≈1.0）尚未跑，
   最终 geomean 会被这些拉低——geomean 3-4x 是**当前 10 程序**的值，不是 30 程序终值。
4. **cholesky/no_feedback 在 2h 预算内不收敛**（超时）。这可能是个子结论
   （「昂贵 kernel + 无反馈」难在预算内收敛），但单点、需更多数据支撑。

---

## 6. 扫描完成后的待办（按优先级）

1. **金标准正确性复核**（用户指定顺序）：gramschmidt → floyd-warshall，
   随后所有 `confirmed 且 > 1.05x` 的 cell。脚本
   `scripts/verify_correctness_golden.sh`：两版都 `-DPOLYBENCH_DUMP_ARRAYS` dump
   全数组、逐值比对、报最大相对误差。**任一 FAIL → 该加速比作废，标注不计入结果。**
2. **random / Bayesian 同预算对照**（LARGE_DATASET）：`scripts/run_search_baseline.py`，
   与 LLM 相同的候选/墙钟预算，不泄漏 LLM 候选。现有的只是小数据 smoke，不可比。
3. 剩余 20 程序跑完，出全 30 程序汇总 + 配对 CI。

---

## 7. 关键路径（绝对路径）

| 内容 | 路径 |
|---|---|
| 逐 cell 原始结果（持续追加） | `/home/hanning/comet/ablation_logs/results.jsonl` |
| 当前汇总 JSON | `/home/hanning/comet/ablation_logs/summary_current.json` |
| 扫描主日志 | `/home/hanning/comet/ablation_logs/sweep_master.log` |
| 每 cell 完整日志 | `/home/hanning/comet/ablation_logs/<program>_<condition>_seed1.log` |
| 每 cell 结果 JSON 快照 | `/home/hanning/comet/ablation_logs/<program>_<condition>_seed1_result.json` |
| 每 run 完整记录（llm_calls、snapshots、pass graph、优化后源码） | `/home/hanning/comet/runs/<ts>_polybench_<program>/` |
| 扫描驱动 | `scripts/run_ablation_matrix.py` |
| 汇总统计 | `scripts/summarize_ablation.py` |
| 金标准复核脚本 | `scripts/verify_correctness_golden.sh` |
| 消融泄漏回归测试 | `tests/test_ablation_no_feedback.py` |
| 上报纪律回归测试 | `tests/test_final_result_gate.py` |

---

## 8. 已交付的代码修复（全部带回归测试；本地 165 tests / 远端 157 tests 通过）

| # | 问题 | 处理 |
|---|---|---|
| 1 | pass whitelist canonicalization（`-mllvm -x` 与 `-x` 不匹配，审计批准的参数被误删） | 交接前已修复，本次复核确认四种写法归一同一 key |
| 2 | confirmed<1.0 不回滚、确认失败上报单次探索值 | `decide_final_result()` 回滚闸门 + utils 原始快照恢复 |
| 3 | 完全没有 no-feedback 消融模式 | `--no-compiler-feedback` + `_strip_compiler_feedback()` |
| 4 | 条件 B 继承 full 的「只能从审计选 flag」规则，被提示词禁止出手 | 改为条件匹配的措辞（真实冒烟发现） |
| 5 | `hotspot_target` 置空击穿默认值致 `re.escape(None)` 崩溃 | 改为删除键；由泄漏测试发现 |
| 6 | 驱动 result JSON 找错目录，会导致整批数据静默丢失 | 改到 per-run 目录查找 |
| 7 | timeout cell 被 resume 无限重试（每次再耗 2h） | timeout 视为确定性终态、不重试；failed 仍重试 |
