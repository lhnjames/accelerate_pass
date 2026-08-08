# COMET 算法规格

_面向论文 Method 一节。本文每条常量与规则都对照 `optimize.py` 当前实现核实过，
文末给出行号索引以便复核。与 `AGENT_PIPELINE_OVERVIEW.md`（2026-07-24，工程视角）
的区别：那份描述的测量与正确性流程已被本次修复取代，以本文为准。_

---

## 1. 问题设定

给定一个 C kernel `K` 与固定编译器 `clang -O3`（LLVM 21.1.8），求一组变换
`(S, F, P)` 使 `T(K) / T(S, F, P)` 最大，同时输出与 `K` 数值等价：

- `S` — 重写后的源码（循环变换、分块、寄存器复用等）
- `F` — `-mllvm` 传入的 pass cost-model 参数
- `P` — 插入的 `#pragma clang loop` 提示

基线 `T(K)` 恒为 `clang -O3` 的墙钟时间，六个实验条件共用同一条编译路径。

---

## 2. 主算法

```
输入: kernel K, 步数预算 N=9, 确认采样数下限 r=3
输出: (S*, F*, P*), 确认加速比

# ── 阶段一：证据收集（整个 run 只做一次）──────────────────────────
E ← collect_all_evidence(K)
    E.passes    ← opt -O3 -debug-pass-manager 抓到的完整 pass 执行序列
    E.ir_diff   ← 逐 pass 在 O0 IR 上单独跑，比对 IR 统计量，区分
                  「执行过」与「真的改了 IR」
    E.remarks   ← -Rpass/-Rpass-missed/-Rpass-analysis 的富 remarks
                  （行列号 + 源码片段 + VF/IC + 失败原因字符串）
    E.perf      ← perf stat 硬件计数器 → 瓶颈分类 → 反向映射到候选 pass
                  ⚠ 本平台不可用，见 §7
    E.options   ← opt --help-hidden 动态发现当前 LLVM 可调的 cost-model 参数
    E.hotspot   ← 调用图打分选出真正该改写的函数（≤4 跳，≤60 个候选）
    E.mode      ← 正确性档位自动判定（§5）

T_base ← 计时(编译(K))
best ← (S₀=K, F₀=∅, P₀=∅, sp=1.0)

# ── 阶段二：agent 循环 ────────────────────────────────────────────
for step in 1..N:
    a ← 决定动作(step)                              # §3
    快照 ← (best.S, best.F, utils 内容)             # 回退栈
    r ← 执行(a, E, history)                         # §4，含正确性门
    history.append(r)
    if r 失败或无改进: 反思 LLM(r) → 结论写回 history
    if r.sp < best.sp × (1 − 20%):                  # 灾难性退化
        恢复(快照)                                   # best.sp 不变，history 不回退
    if r.sp > best.sp: best ← r
    if a == "done": break

# ── 阶段三：收尾 ──────────────────────────────────────────────────
if S 与 F 从未一起测过: 补测 compound
sp* ← confirm(K, best)                              # §6
report(sp*, status, IQR, n, CV)
```

---

## 3. 动作选择

| 步 | 规则 |
|---|---|
| 1 | 强制 `try_flags` |
| 2 | 强制 `rewrite_source` |
| ≥3 | 元规划 LLM 每 3 步给一次序列；同一动作连用 2 次触发强制换道 |

前两步强制的目的是保证参数通道与源码通道各自都被独立测过一次，避免 LLM
在第一步就锁死到单一通道。

消融条件通过覆盖这个规则实现（§8）。

---

## 4. 动作空间（三通道）

### 4.1 `try_flags` — pass cost-model 参数

只允许调 `-mllvm` 的**阈值类**参数（`--slp-threshold`、`--licm-mssa-optimization-cap`
等），黑名单拦截绕过 cost model 本身的参数（如强制向量宽度）。两阶段搜索：

- **Phase A** 每个候选参数单独测一次（快速筛选）
- **Phase B** 取 Phase A 最好的 2–3 个组合联合测（参数间有交互，各自最优 ≠ 组合最优）

候选值来自 `E.options` 的动态发现，不是硬编码列表。预算：单步 20 分钟
（`TRY_FLAGS_STEP_BUDGET_S`），单次网格搜索 2 小时（`PARAM_SEARCH_BUDGET_S`）。

### 4.2 `try_pragma` — 循环提示

在目标循环前插入 `#pragma clang loop ...`，转成 `!llvm.loop` 元数据。

定位规则（LLM 给出 `loop_prefix` 字符串，需**唯一**匹配一行 `for`）：

1. 候选行限定在**目标 kernel 函数体内**
2. 归一化前缀匹配 → 原始前缀匹配 → **归纳变量匹配** → 关键词子集匹配
3. 任一档命中 >1 处即判歧义，**拒绝**，且不下探更宽松的档

第 1 条与归纳变量档是本次新增：跨函数的同名循环头（`init_array` 与 `kernel_xxx`
里都有 `for (i = 0; i < ni; i++)`）此前导致 108 次合法 pragma 被误判为歧义而丢弃，
是全语料最大的单项浪费。

### 4.3 `rewrite_source` — 源码重写

两阶段：决策 LLM 只选策略与目标函数；另一个分析+实现 LLM 看到真实源码后
写出代码。唯一允许改变代码结构的通道，收益上限最高，因此有两层正确性检查（§5）。

**多函数联合重写**：热点分数按降序排列，取相邻分数**相对下降幅度最大**的断点，
断点之前的候选一起改写；若最大断点幅度本身 <8%（分布太平、无结构性分界），
退化为只改最高分的一个。

---

## 5. 正确性协议

### 5.1 档位自动判定

对参考二进制跑两次，按输出性质选择：

| 条件 | 档位 | 比较方式 |
|---|---|---|
| 确定 + 存在非整数值 | `numeric` | 相对容差 ε=1e-4，且不小于输出量化步长 |
| 确定 + 全为整数值 | `hash` | 逐字节 SHA256 |
| 确定 + 无数值 | `hash` | 同上 |
| 不确定但数值稳定 | `numeric` | 同上 |
| 不确定且数值不稳定 | `exit_only` | 仅退出码 |

**为什么不是「numeric 最强」**：`numeric` 容忍相对误差，对浮点 kernel 正确
（向量化必然重结合），但对离散输出**严格更弱**——`telecom_crc32` 打印 ~4e9 的
校验和，1e-4 相对容差允许 ±40 万误差。判据是「这个值能否合法变动」。

### 5.2 打印精度

校验构建使用 `-DDATA_PRINTF_MODIFIER="%0.12lf "`（PolyBench 默认 `%0.2lf`）。

dump 格式决定检查能分辨的最小差异。`%0.2lf` 下量化步长是 0.01，而 1e-4 相对容差
在 59.48 上只有 0.006——比输出能表示的还细，检查退化成「最后一位打印数字是否变了」。
本研究 23 次「数值不符」拒绝全部是最后一位差 1，即向量化重结合。

提高精度而非放宽容差：实测放宽容差会让 `jacobi-1d`、`seidel-2d` 的门失效
（清空 kernel 仍判通过）；提高精度则两端同时解决——重结合落在 ~1e-13 相对被放行，
清空 kernel 差在首位数字被拦下。

计时构建走 `-DPOLYBENCH_TIME`，不调用 `print_array`，耗时不受影响。

### 5.3 两层数据规模

1. `SMALL_DATASET`，容差 ε
2. `STANDARD_DATASET`（超时自动降级 `MINI`），容差 2ε

### 5.4 参考程序健康检查

建库前先跑一次参考二进制，以下任一情况判为不健康并报错：退出码非 0、输出为空、
输出含 I/O 错误串（`can't open` / `no such file` 等）。

无此检查时，`bzip2_encode` 在缺输入文件的节点上 **exit 0 + 空输出**，hash 模式
拿空和空比判通过，0.96 ms 的空转被记成 85 ms 基准的结果。

### 5.5 门的有效性验证（变异测试）

把 kernel 函数体替换为 `return;`，门**必须**判失败。全部 30 个 PolyBench 程序逐个测：
28 个有效，`heat-3d` 与 `seidel-2d` 无效（dump 的数组与 kernel 无关，任何精度下
清空 kernel 输出都逐位相同），已从研究中排除。

---

## 6. 测量协议

### 6.1 配对交替测量

```
warm(base); warm(cand)
repeat: t_b ← 计时(base); t_o ← 计时(cand); ratios.append(t_b / t_o)
```

交替而非分批，抵消温控降频与负载漂移。

### 6.2 采样量：时长 + 方差双驱动

```
n₀ ← clamp(ceil(500ms / 单次耗时), r, 51)      # 时长驱动，取奇数
采样直到 n ≥ n₀ 且 (IQR半宽/中位数 ≤ 5% 或 n ≥ 51 或 墙钟 ≥ 300s)
```

只按时长缩放是不够的——决定中位数需要多少样本的是**方差**。`2mm` 基线 3.7 秒
按时长规则只拿到下限 3 个样本，而其优化版逐次变异 42.8%、三个比值跨越
[5.00, 10.53]；发布值 9.18x 是该区间中点，空闲核独立复测为 4.82/5.75。

### 6.3 报告口径

`final_speedup` = n 次配对比值的**中位数**。最大值保留为 `best_observed_speedup`
但不作为主数字。

对带噪样本取最大值在 n=3 时按构造上偏约 0.85σ。该口径此前使条件 ① 的 geomean
虚高 9.7%、条件 ④ 虚高 5.9%，短基准子集虚高 22%（中位数 0.871 报成 1.063，符号相反）。

### 6.4 显著性

`significant_gain` ⇔ IQR 整体位于 1.0 之上 **且** 每一次配对均为正。

旧判据仅要求中位数 >1.0，`crc32` 因此在 IQR = [0.853, 1.635]（连符号都未定）时被标显著。

### 6.5 终态分类

| status | 含义 | 报告值 |
|---|---|---|
| `confirmed` | 确认测量完成 | 中位配对比值 |
| `baseline_only` | 预算用尽，无候选通过确认 | 1.0（**是数据点，不可丢弃**） |
| `incorrect` | 产物未通过正确性比对 | 1.0 |
| `exploratory_only` | 确认无法进行 | 探索期值 |

`baseline_only` 与 `incorrect` 一度因缺 `confirmed_median` 字段被聚合脚本丢弃，
等于只统计成功案例——条件 ① 在 cBench 上 19 个程序只有 10 个进入统计。

---

## 7. 反馈通道的实际可用性

`feedback_used` 由**运行时探测**生成，不由命令行参数推断：

| 通道 | 本平台状态 |
|---|---|
| pass pipeline / IR diff / remarks / 动态参数发现 | ✅ 可用 |
| perf 硬件计数器 | ❌ `perf_event_paranoid=4` 封禁 |
| VTune | ❌ Intel x86 工具，节点是 aarch64 |

因此本研究中条件 ③ 的实际反馈是 `compiler`，**不是** `compiler+hardware`。探测失败
时按不可用处理（fail-closed）。

---

## 8. 消融条件定义

| 条件 | 命令行 | 屏蔽/强制了什么 |
|---|---|---|
| ① rewrite-only | `--rewrite-only --no-compiler-feedback` | 每步强制 `rewrite_source`；屏蔽 13 类编译器/硬件证据 |
| ② no-compiler-feedback | `--no-compiler-feedback` | 自由选动作；屏蔽同上 13 类 |
| ③ full system | （无） | 自由选动作 + 完整编译器侧证据 |
| ④ params-only | `--params-only` | 每步强制 `try_flags` |

`--no-compiler-feedback` 屏蔽的 13 项：`kernel_remarks, rich_remarks, missed_counts,
kernel_passes, top_passes, targeted_passes, discovered_opts, pass_graph, kernel_ir,
ir_diff_info, ir_pass_diffs, baseline_perf, baseline_stats`。

**提示词对称性**：条件 ② 下若照搬 full 条件的「只能从 audit 输出里选参数」规则，
等于要求 LLM 从空集合里选，实测它会直接交空 flags 列表——那测到的是「我们禁止了
它出手」而非「没有反馈时它能做到多少」。因此 ② 明确允许凭自身 LLVM 知识提参数，
越界参数仍由黑名单拦截。两条件动作空间一致，差的只有证据。

---

## 9. 两个外部 baseline

### 9.1 OC — OpenCode + DeepSeek

通用 CLI agent，与 COMET 同模型（deepseek-v4-pro）、同步数（9 轮）、同确认协议。
每轮给它 `./measure.sh` 的结果，它直接编辑 `kernel.c`。会话跨轮延续。

harness 在花掉 9 轮之前先探一次 API 端点——**key 存在不等于 key 能用**：余额耗尽时
每轮返回 402、opencode 退出 0 且未编辑任何文件，21 个任务曾被记为 1.0000x 的「结果」。

### 9.2 PO — AutoPass 复现 (arXiv 2606.20373)

四-agent 架构（Score / Analysis / Reasoning / Evaluation），R3 轮预算，74-pass 目录，
严格 `t(P) < t(P*)` 才接受否则回退。

关键实现细节：
- pass 流水线按 opt 的嵌套语法显式构造，每个 loop pass 单独一个适配器
  （与扁平列表的隐式适配语义一致，已验证 IR 逐字节相同）
- `instcombine` 一律写作 `instcombine<no-verify-fixpoint>`。其 fixpoint 校验器在
  非标准位置会直接 `LLVM ERROR` abort——这曾吞掉 147 轮中的 19 轮，4 个程序三轮
  全废却被记为 1.000x
- 候选下发前用 kernel 自身的 IR 做 opt 预检，不合法的项逐个剔除而非废掉整轮

---

## 10. 实现索引

| 内容 | 位置 |
|---|---|
| 主循环 / 强制动作 / 回退 | `optimize.py:5284-5420` |
| 终态判定 | `optimize.py:decide_final_result` |
| 配对确认 + 自适应采样 | `optimize.py:confirm_result_external`、`_adaptive_confirm_runs` |
| 正确性档位与比较 | `src/correctness.py:detect_correctness_mode`、`compare_numeric`、`output_quantum` |
| 参考健康检查 | `src/correctness.py:reference_health` |
| pragma 定位 | `optimize.py:_apply_pragma_hints` |
| 参数搜索 | `tune_param.py:run_param_round`、`discover_options_from_help` |
| 热点选点 | `src/hotspot.py:rank_all_reachable` |
| 硬件计数器可用性探测 | `optimize.py:hardware_counter_availability` |
| AutoPass 复现 | `scripts/passorder_search/run_autopass.py`、`measure_lib.py` |
| OpenCode baseline | `scripts/opencode_harness/` |

## 11. 关键常量

| 常量 | 值 | 位置 |
|---|---|---|
| 步数预算 | 9 | `--rounds` |
| 灾难性退化阈值 | 20% | `config.runtime.catastrophic_slowdown_threshold` |
| 单步 try_flags 预算 | 20 min | `TRY_FLAGS_STEP_BUDGET_S` |
| 单次参数网格预算 | 2 h | `PARAM_SEARCH_BUDGET_S` |
| 确认目标时长/侧 | 500 ms | `_CONFIRM_TARGET_MS` |
| 确认采样上限 | 51 | `_CONFIRM_MAX_RUNS` |
| 确认目标相对半宽 | 5% | `_CONFIRM_TARGET_REL_SPREAD` |
| 确认墙钟预算 | 300 s | `_CONFIRM_VARIANCE_BUDGET_S` |
| 正确性容差 | ε=1e-4（第二层 2ε） | `_correctness_check` |
| 校验打印精度 | `%0.12lf` | `DUMP_PRECISION_FLAG` |
| 联合重写断点阈值 | 8%（`min_gap_pct`，默认参数） | `src/hotspot.py:356` |
