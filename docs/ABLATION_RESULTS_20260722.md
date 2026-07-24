# 编译器反馈消融实验（Full vs No-compiler-feedback）— 2026-07-22

本文件只记录**协议**和**已经跑出来的实测数字**。所有数字都标注了它属于哪一类：
`baseline` / `exploratory` / `confirmed` / `rolled_back`。未完成的部分在文末
「未完成事项」里明确列出，不做任何推断性填空。

---

## 1. 实验条件定义

| | 条件 A：Full | 条件 B：No compiler feedback |
|---|---|---|
| kernel 源码 | ✅ | ✅ |
| 编译器版本 / `-O3` 编译命令 | ✅ | ✅ |
| 基线时间 | ✅ | ✅ |
| correctness contract | ✅ | ✅ |
| 本 run 自己的候选历史 + 失败反思 | ✅ | ✅ |
| pass remarks（passed/missed） | ✅ | ❌ |
| missed transformation 明细 | ✅ | ❌ |
| O3 pass 清单 / pass purpose / status | ✅ | ❌ |
| IR 文本、IR diff、pass graph | ✅ | ❌ |
| `opt-21 --help-hidden` 发现的 debug 参数 | ✅ | ❌ |
| LLM pass/runtime 审计阶段 | ✅ | ❌（整个阶段跳过） |
| perf 硬件反馈（IPC / cache / branch / 瓶颈） | ✅ | ❌ |
| profile 驱动的热点重定向 | ✅ | ❌ |

两个条件**匹配**的项：同一批程序、同一台机器、同一 CPU 绑核、同一编译器、
同一模型（DeepSeek v4 pro）、同一 rounds/runs 预算、同一确认重复次数、
同一 correctness 判定。**唯一的差别是反馈通道本身。**

### 1.1 屏蔽是如何强制的（不是靠人工检查）

`optimize.py::_strip_compiler_feedback()` 在 `collect_all_evidence()` 之后
一次性把上表所有 ❌ 通道在 `ev` 里清空。之所以在这一个点上做，是因为下游
所有消费者——`_build_remarks_and_targeted_passes()`、`_build_evidence_sections()`、
`_build_agent_prompt()`、`_pass_runtime_evidence_text()`、以及 try_flags 的
auto-supplement——都只从这一个 dict 取内容，因此屏蔽是**构造上完备**的，
而不是靠逐个 prompt 去记得加判断。

额外两处强制：

- **审计阶段整体跳过**（`NO_COMPILER_FEEDBACK` 门控）。只隐藏审计文本是不够的：
  审计排好序的 flag 会经由 `audit_params` 直接进入 `flag_specs`，等于把候选
  偷偷传给条件 B。
- **auto-supplement 自动失效**：它读的正是被清空的 `targeted_passes` /
  `discovered_opts`。

证据不是自证的，`tests/test_ablation_no_feedback.py` 在 `ev` 的每个反馈通道里
埋了唯一标记串，然后断言：这些标记**确实出现**在 Full 条件渲染出的 prompt 里，
且**一个都不出现**在条件 B 的 prompt 里。第一条断言是必要的——否则「标记没泄漏」
可能只是因为标记根本没进过 prompt，那样这个测试就什么也证明不了。

### 1.2 一个必须记录的公平性修正

第一次 3mm 条件 B 冒烟里，LLM 第 1 步回复：

> 「必须选择 try_flags，但系统规则要求只能从 audit 输出的 debug 参数中选择 flag，
> …为避免凭空发明 flag 违反规则，只能输出空 flags 列表。」

原因是 try_flags 的提示词里那条「只能从 audit 输出里选，不要凭空发明 flag」规则
**在两个条件下都发**。可条件 B 按定义没有审计，这条规则就退化成「不准提任何参数」。
那样测到的不是「没有编译器反馈时 LLM 能做到多少」，而是「我们用提示词禁止了它出手」，
会人为压低条件 B。

已修正：条件 B 改为明确允许 LLM 凭自身 LLVM 21 知识提参数，并明说「不要因为缺少
证据就交空列表」。越界参数仍由 `is_cost_model_override()` 黑名单拦截（force/disable
类），所以**两个条件的动作空间一致，差的只有证据**。回归测试见
`TestTryFlagsRuleIsConditionMatched`。

> 注意：本文件下方所有条件 B 的数字，都是在这条修正**之后**跑的。修正之前的那次
> 3mm 冒烟只用于验证链路，不进入任何统计。

---

## 2. 测量与上报纪律

- speedup = `baseline_median / candidate_median`，由 `confirm_result_external()`
  以 **baseline/candidate 交替测量 + 配对比值中位数**得到，外部墙钟计时
  （不解析程序自身 stdout）。交替可以让两侧共享同一段测量窗口、互相抵消系统漂移。
- 正式确认重复次数 `--runs`，本次扫描为 **5**（协议下限 3）。
- **回滚闸门**（`decide_final_result()`，新增）：只有「确认过且 ≥ 1.0」才允许上报
  非 1.0 的数字。其余一律记 **1.0000x（纯 -O3）**并记录原因：
  - `rolled_back_regression`：确认 < 1.0，候选实际是回归 → 丢弃 flags/重写。
  - `rolled_back_unconfirmed`：确认测量没跑成，只剩单次探索测量 → 不足以作为正式结果。
  - `baseline_only`：从未产生候选。
- 探索期单次峰值仍然保留，但只存进 `exploratory_speedup` 字段，**永远不进正式表**。

这条纪律直接来自交接里的 3mm 教训：`-licm-max-num-uses-traversed=24` 单次测得
**1.0492x**，交替确认却是 **0.9886x**。修改前的代码会把 `confirmed if ok else
best_speedup` 上报，两种坏情况（确认为回归、确认没跑成）都会把一个未成立的数字
写进结果。回归测试见 `tests/test_final_result_gate.py`。

---

## 3. 已验证的代码修复

| # | 问题 | 状态 |
|---|---|---|
| 1 | pass whitelist canonicalization：`-mllvm -licm-...` 与 `-licm-...` 不匹配，审计批准的参数被误删 | **交接前已修复**（commit `fd76e91`），`tests/test_canonical_flag.py` 覆盖；本次审计复核确认 `_canonical_flag_key()` 对 `--`/`-`/`=value`/内嵌 `-mllvm` 四种写法归一到同一 key |
| 2 | confirmed < 1.0 不回滚，仍上报；确认失败时上报单次探索值 | **本次修复**：`decide_final_result()` + 回滚闸门 + utils 原始快照恢复 |
| 3 | 完全没有 no-compiler-feedback 消融模式 | **本次新增**：`--no-compiler-feedback` + `_strip_compiler_feedback()` |
| 4 | 条件 B 继承 Full 的「只能从审计里选 flag」规则，被提示词禁止出手 | **本次修复**（见 §1.2） |
| 5 | `hotspot_target` 置空会击穿 `ev.get(k, kernel_name)` 默认值并在 `re.escape(None)` 崩溃 | **本次修复**：该键改为删除而非置空；由 §1.1 的测试发现 |

测试规模：本地 **157 passed**（改动前 136），远端 **157 passed (10 skipped)**。

---

## 4. 正确性验证（独立于流水线自身）

对 3mm 的 LLM 源码重写（循环交换 ijk → ikj）做了**独立金标准复核**，不依赖
COMET 自己的 correctness gate：原始版与重写版都用 `clang-21 -O3 -DLARGE_DATASET
-DPOLYBENCH_DUMP_ARRAYS` 编译，dump 全部数组后逐值比对。

```
ref bytes: 8790434   cand bytes: 8790434
RESULT: BIT-IDENTICAL
counts: 880000 880000
max relative error: 0.000e+00
VERDICT: PASS
```

880,000 个浮点值**逐位完全相同**。循环交换只改了嵌套顺序，每个 `E[i][j]` 对 k 的
累加顺序未变，所以浮点结果按位一致，符合预期。

---

## 5. 实测结果

**状态：扫描进行中。** 完成的 cell 会持续追加到
`/home/hanning/comet/ablation_logs/results.jsonl`，汇总由
`scripts/summarize_ablation.py` 生成。本节在扫描完成前不填写任何汇总统计，
以免出现样本量不足的表被后续引用。

### 5.1 扫描配置

| 项 | 值 |
|---|---|
| 主机 | `ubuntu@132.145.22.86`，4 核 aarch64 Neoverse-N1 |
| 编译器 | `/usr/bin/clang-21`（21.1.8） |
| 程序 | **全部 30 个 PolyBench/C 4.2 kernel** |
| 数据集 | `LARGE_DATASET` |
| 条件 | full, no_feedback |
| seed | 1（单 seed，用户 2026-07-23 决定：只做条件对比，不做多 seed 稳定性） |
| 预算 | `--rounds 3 --quick-check`，每 cell 相同 |
| 确认 | `--runs 5`，交替 baseline/candidate |
| 绑核 | `--pin-cpu 2` |
| 并发 | **1**（串行）。4 核机器上并行跑会让 memory-bound kernel 争抢内存带宽，污染计时；为保证计时可信度牺牲吞吐 |
| cells | 30 × 2 × 1 = 60 |

**范围演进**：最初跑的是 9 个优先 kernel（访存受限/难向量化）× 2 条件 × 3 seed = 54 cell，
seed 1 已完整完成（见 §5.3）。随后用户决定：**扩到全部 30 个程序、但只做单 seed**
（放弃 seed 2/3 的稳定性维度，聚焦 Full vs No-feedback 的横向覆盖）。因此现在是
30 程序 × 2 条件 × seed 1 = 60 cell。9 优先 kernel 的 seed-1 结果通过驱动的 resume
机制原样保留（20 个已完成 cell 直接跳过，含 cholesky/no_feedback 的 timeout 终态
不再重试、及 3mm 的 2 个 seed-2 额外确认样本）。

> 单 seed 的代价：本次**无法**报告跨 seed 的稳定性或按 seed 的 bootstrap CI。
> §5.3 里 seed-1 全 9 程序的配对统计仍然有效（那个 CI 反映的是跨程序离散度）。
> 全 30 程序完成后，CI 会覆盖 30 个程序对，横向证据更强，但纵向（随机种子）稳定性
> 仍是未测维度，需在结论里明说。

### 5.3 seed-1 九优先程序配对结果（中间结果，大加速待金标准复核）

seed 1 的 9 个优先程序两个条件全部完成（cholesky/no_feedback 超时除外）。
逐 cell 确认加速比（`--runs 5` 交替确认）：

| program | full | no_feedback | 备注 |
|---|---:|---:|---|
| 3mm | 9.96x | 10.38x | 大加速，循环交换；待复核 |
| nussinov | 1.10x | 1.10x | 平 |
| cholesky | 1.01x（边缘）| **timeout** | no_fb 未在 2h 预算内收敛 |
| floyd-warshall | 1.00（baseline_only）| **1.43x** | no_fb 明显更好；full 未找到候选 |
| gramschmidt | 21.38x | 19.68x | 大加速；**最高复核优先级** |
| covariance | 17.18x | 17.30x | 大加速；待复核 |
| correlation | 17.30x | 16.97x | 大加速；待复核 |
| adi | 1.009x（边缘）| 1.00（baseline_only）| 本质都≈1.0 |
| seidel-2d | 1.023x | 1.019x | stencil，小增益 |

`scripts/summarize_ablation.py` 的 seed-1 汇总（`ablation_logs/summary_seed1.json`）：

| 指标 | full | no_feedback |
|---|---:|---:|
| 可用 cell | 9 | 8（cholesky 超时） |
| geomean（程序中位数） | 3.47x | 4.20x |
| 成功率 ≥1.0 | 100% | 100% |
| 严格增益 >1.01 | 78% | 88% |
| IQR 完全高于 1.0 的真增益 | 89% | 88% |

**配对比较（8 个两条件都完成的程序）**：
- geomean full = 4.05x，no_feedback = 4.20x
- 配对比值 full/no_feedback = **0.9644**，bootstrap **CI95 = [0.96, 1.02]**
- 逐条 head-to-head：full 赢 5，no_feedback 赢 3

**读法**：配对比值的 95% CI **跨越 1.0** → 在这 8 个程序、seed 1 上，full 与
no_feedback **没有统计显著差异**。一个细节：full 逐条赢的程序更多（5:3），但
no_feedback 赢的时候赢得更大（floyd-warshall 1.43x vs full 1.0），两种效应抵消。
这是「No-feedback ≈ Full」方向的第一个量化支撑，但**大加速（~17-21x）主导了 geomean，
必须先过金标准复核**；且这是单 seed，跨 seed 稳定性未测。

### 5.2 单个已完成的链路验证结果（不是统计样本）

下面这一条来自公平性修正**之前**的链路冒烟，仅用于证明条件 B 端到端可用，
**不进入任何统计**：

| program | condition | seed | baseline_ms | confirmed | status | 备注 |
|---|---|---|---:|---:|---|---|
| 3mm | no_feedback | 1 | 9081.65 | **8.9229x** | confirmed (n=3) | 循环交换 ijk→ikj；正确性金标准复核为逐位相同（§4）。探索期单次 10.0022x 未采用 |

这条结果本身有一个值得注意的方向性信号：**在完全没有编译器反馈的条件下**，
LLM 仅凭源码就找到了 3mm 的经典循环交换，拿到接近 9x 的确认加速。这暗示对
PolyBench 这类结构规整的 kernel，**源码改写通道对编译器反馈的依赖可能远低于
参数调优通道**。但这是 n=1、且在公平性修正前跑的，**不能作为结论**，需要等
正式扫描的配对数据。

---

## 6. 未完成事项 / 不能证明的结论

1. **正式消融统计尚未产出**：54 个 cell 的扫描在写此文时仍在运行（串行，单 cell
   约 30–45 分钟，全量约 30–40 小时）。§5 只有配置和一条链路验证。
2. **random / Bayesian 同预算对照尚未在 LARGE_DATASET 上重跑**。已有的
   `results/baseline_search_3mm_smoke_5seed_20260722/` 是**小数据 smoke**
   （每 seed 仅 2 个候选、`runs=1`），不能与本次 LARGE_DATASET 的 LLM 结果直接比。
   为保护计时可信度，没有与消融扫描并行运行。
3. **CBench / SPEC 未纳入本次消融**，按交接的优先级排在 PolyBench 之后。
4. **不能证明**「编译器反馈对 LLM 优化有/无显著帮助」——这正是扫描要回答的问题，
   在配对数据出来之前，任何方向的结论都没有证据。
5. 条件 C（无硬件反馈单独消融、无 reflection、固定预算）**未实施**。当前
   `--no-compiler-feedback` 把编译器反馈与硬件反馈**一起**移除，这是交接允许的
   合并定义，但因此**无法区分**两者各自的贡献。若要拆分，需要再加一个只屏蔽
   `baseline_perf` 的开关。

---

## 7. 关键路径（绝对路径）

| 内容 | 路径 |
|---|---|
| 消融逐 cell 结果（持续追加） | `/home/hanning/comet/ablation_logs/results.jsonl` |
| 扫描主日志 | `/home/hanning/comet/ablation_logs/sweep_master.log` |
| 每 cell 完整日志 | `/home/hanning/comet/ablation_logs/<program>_<condition>_seed<N>.log` |
| 每 cell 结果 JSON 快照 | `/home/hanning/comet/ablation_logs/<program>_<condition>_seed<N>_result.json` |
| 每 run 完整记录（含 llm_calls.jsonl、snapshots、pass graph） | `/home/hanning/comet/runs/<timestamp>_polybench_<program>/` |
| 链路验证 run（3mm no_feedback） | `/home/hanning/comet/runs/2026-07-22_18-11-36_polybench_3mm/` |
| 扫描驱动 | `scripts/run_ablation_matrix.py` |
| 汇总统计 | `scripts/summarize_ablation.py` |
| 消融泄漏回归测试 | `tests/test_ablation_no_feedback.py` |
| 上报纪律回归测试 | `tests/test_final_result_gate.py` |
