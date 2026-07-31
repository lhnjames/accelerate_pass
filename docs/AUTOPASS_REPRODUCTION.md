# AutoPass 复现说明与测量 bug 复盘

对照论文：**AutoPass: Evidence-Guided LLM Agents for Compiler Performance Tuning**
(arXiv 2606.20373, 2026-06-18)

本文档说明 (1) 我们复现了论文的哪些部分、哪些没有复现及原因，(2) 复现过程中发现的
三个测量 bug——它们曾使 PO 条件的每一个结果都被系统性低估到 0.2x–0.7x。

---

## 1. 论文做了哪些实验

论文评测部分共 12 类实验：

| # | 实验 | 核心结果 |
|---|---|---|
| 1 | 引入案例 QSort / BitCount | AutoPass 平均 1.259x，优于 PGO 系与 OpenTuner |
| 2 | **RQ1 总体 geomean (Table 5)** | x86-64 **1.043x** / ARM64 **1.117x** |
| 3 | **RQ2 迭代收益 R1 vs R3 (Table 6)** | x86: 1.010x(18胜13负) → 1.040x(25胜6负)；ARM64: 1.004x(15胜16负) → 1.109x(27胜4负) |
| 4 | **收敛性 (Figure 3)** | R1 1.010x → R2 1.033x → R3 1.040x，R4–R6 仅再涨 <0.4%，第3轮即饱和 |
| 5 | 跨架构自适应 (Table 7) | x86 与 ARM 产出的 pipeline 编辑相似度 0.917，非同一套策略照搬 |
| 6 | Pass 覆盖率变化 (Figure 4) | ARM64 更激进使用 SLP 向量化（41.9% vs x86 32.3%） |
| 7 | RQ4 Score Agent vs PGO 热点 (Table 8) | Top-10 时 Score Agent 1.0333x ≈ 全量搜索，尽管与 PGO 选中函数仅 35.5% 重叠 |
| 8 | **RQ5 四-agent 消融 (Table 9)** | Reasoning Agent 最关键（去掉后 R3 仅 0.870x 且无法恢复）；Evaluation Agent 决定迭代鲁棒性（去掉后卡在 0.961x）；Analysis Agent 影响收敛速度但非必需 |
| 9 | **RQ6 可解释案例 (Sec 6.6, QSort)** | R1 过度展开 → L1 miss +133%、退化 0.77% → R2 收敛参数 → R3 达 1.028x |
| 10 | LLM 后端泛化 (Table 10) | DeepSeek/GPT-4o/Qwen3/Gemini 的 R3 均达 1.04–1.11x，框架与模型无关 |
| 11 | 无 rollback 鲁棒性 | AutoPass 仍为正（x86 1.040x，仅 6 次退化）；OpenTuner 波动极大 |
| 12 | 对比 500 轮 OpenTuner | AutoPass 3 轮 1.109x vs OpenTuner 500 轮 1.126x（ARM64），少 166 倍评估次数 |

论文设置：LLVM **17.0.6**，74 个 pass，序列最长 107；x86-64 (i9-11900K) 与
ARM64 (树莓派5 Cortex-A76)；每个二进制跑 5 次取几何均值；benchmark 为
cBench 31 + PolyBench 30 + CoreMark + MiniFE + LULESH。

## 2. 我们复现了什么

**已复现（`scripts/passorder_search/run_autopass.py`）：**

- **四-agent 架构**
  - *Score Agent*：调用图可达函数按 #blocks/#loops/#calls/#condbranch 打分选目标
  - *Analysis Agent*：`-fsave-optimization-record=yaml` 提取 remarks → LLM 归一化为 JSON
  - *Reasoning Agent*：从 74-pass 目录提出 pass 顺序 **+ 数值参数**，带 difflib 确定性修复
  - *Evaluation Agent*：编译 + 计时 + 正确性校验，**严格** `t(P) < t(P*)` 才接受，否则回退
- **74-pass 目录**（`pass_list_autopass.py`，逐个对 `opt-21 --help-hidden` 核验）
- **pass 参数调优**：论文 QSort 案例调的 `unroll_count / unroll_threshold /
  inline_threshold / slp_threshold` 全部包含在 `TUNABLE_PARAMS` 内
- **R3 轮次预算**（论文主结果就是 3 轮）
- **rollback 策略**

**未复现及原因：**

| 论文实验 | 未做原因 |
|---|---|
| #1 #5 #6 跨架构 x86 vs ARM | 只有 ARM64 一个平台 |
| #7 Score Agent vs PGO 热点 | 无 PGO baseline 实现 |
| #10 换 LLM 后端 | 目前只接入 DeepSeek |
| #11 #12 vs OpenTuner | 无 OpenTuner baseline |
| CoreMark / MiniFE / LULESH | 语料库中无此三者 |
| 硬件计数器反馈 | 两台测量节点 `perf_event_paranoid=4` 封禁 perf，改动它是主机级安全设置，不应擅自修改。Evaluation Agent 仅看墙钟时间与正确性 |

**已知实现差异（论文未披露、由我们自行决定的部分，均在代码注释中标注）：**
Score Agent 的打分权重公式、Analysis Agent 的 JSON schema、difflib 相似度阈值 0.6、
74-pass 目录的具体构成（论文只说数量为 74，未列出）。

---

## 3. 三个测量 bug（重要复盘）

修复前，PO 条件**每一个**程序都报告严重退化（gemm 0.163x、syrk 0.493x、2mm 0.205x…）。
此前 `ABLATION_STUDY_LATEST_20260727.md` 将其归因为「22-pass 目录 + O0 codegen 本质弱于
-O3」。目录偏弱确实存在但只是次要因素，**主因是三个 harness bug**：

### Bug 1：codegen 跑在 -O0

第 3 步用 `clang -c kernel_opt.ll`，而 clang 在**未给 -O 标志时默认 -O0 codegen**——
不做指令调度、寄存器分配质量差、无机器级 peephole。

> 注意 `clang -O3 -c` 并**不是**修法：它会在候选 pass 顺序之上重跑整条 -O3 IR pipeline，
> 恰好抹掉本 harness 唯一想测量的变量。`llc` 只做后端 codegen，才是正确工具。

同一份**真实 -O3 IR** 分别喂给两者（gemm，baseline 0.577s）：

| codegen 方式 | 耗时 | 相对 baseline |
|---|---|---|
| `clang -c`（原实现） | 1.621s | **0.36x** |
| `llc -O3`（修复后） | 0.586s | **0.98x** |

### Bug 2：frontend 跑在 -O1

`-O1 -Xclang -disable-llvm-passes` 与 `-O3 -Xclang -disable-llvm-passes` 产出的
IR **函数属性不同**（inline hint 等），而后续 opt pipeline 依赖这些属性。

后果极其隐蔽：用 -O1 frontend 时，**即使把完整的 `default<O3>` pipeline 喂给 opt，
也只能达到 0.69x**——即 harness 无论给它多好的 pass 顺序，都无法复现自己的 baseline。
换成 -O3 frontend 后同一条 pipeline 达到 **1.01x**（理应如此）。

### Bug 3：拿 stale baseline 做分母

`baseline_ms` 只在 `prepare_task` 阶段测一次，之后每一轮都拿它做除数。但各轮在数小时后
执行，期间机器负载已完全不同：实测同一个 `ref_bin`，prepare 时 **139ms**，某轮运行中
**618ms**。这会把一个诚实的 0.82x 候选报告成 **0.18x**。

危害不止于数字失真——**它污染了搜索过程本身的 accept/reject 决策**。现已改为每轮将
`ref_bin` 与候选**配对测量**（与 `confirm_result_external` 对最终结果的做法一致）。

### 附加 bug：P\* 初值是未经测量的目录

`best_passes` 初值被设为 74-pass 目录并**假定**其加速比为 1.0，但该值从未被实测。
当没有任何一轮超过 1.0x（常见情况），finalize 就会编译这个目录并报告其真实速度——
于是「没找到比 -O3 更好的方案」被记录成 0.2x–0.6x 的退化。

现已改为 P\* 初值 = LLVM 自带 `default<O3>` pipeline（这才是候选真正要击败的对象），
并在结果中输出 `no_improvement_over_O3` 标志。

### 修复后验证（gemm，配对测量）

| 配置 | 加速比 |
|---|---|
| `default<O3>`（P\* 初值） | **1.0022x** ← 精确复现 -O3，理应如此 |
| 22-pass 目录 | **0.8135x** ← 有意义的「目录弱于 -O3」结论 |

端到端跑通后 gemm 的表现：3 轮均未击败 -O3 → 正确回退至 P\* → 确认 **1.0014x**、
`no_improvement_over_O3: true`。这与论文自身结果一致——论文 PolyBench x86 的
geomean 也仅 1.009x，即大多数程序确实找不到比 -O3 更好的 pass 顺序。

---

## 4. 状态

所有 49 个 PO 任务已作废并重新入队，用修复后的 harness 重跑。
**修复前产生的任何 PO 数字都不可引用。**

其余条件（①②③④/OC）不经过 passorder harness，不受这三个 bug 影响。
