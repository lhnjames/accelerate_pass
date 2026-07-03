# COMET 信息文档（面向论文素材整理，非论文格式）

**日期**: 2026-07-03
**范围**: 描述当前实际运行的系统 —— 入口 `optimize.py`，配合 `tune_param.py`（Param 通道）、`tune_source.py`（Source 通道）、`src/*.py`（编译器/性能/日志基础设施）。
**重要提示**: `docs/aim.md` 和 `docs/implementation_report_for_advisor.md` 描述的是一套**已经不存在的旧架构**（`run.py`、`mcts_core.py`、`mcts_expansion.py`、`reward_functions.py`、`pass_concept_mapper.py`、`dynamic_pass_extractor.py`），仓库里已找不到这些源文件（只剩 `__pycache__` 里的 `.pyc` 残留）。写论文/汇报时不要引用这两份文档的架构描述，只能引用其中仍然成立的方法论讨论（已在下文标注）。详见第 8 节。

---

## 1. 系统是什么

COMET 是一个 LLM agent，目标是在 `clang -O3 -march=native` 的基础上进一步压榨性能（"beyond-O3"范式）。给定一个 kernel 源文件（当前以 PolyBench/C 为对象），agent 通过多轮迭代，从三个正交通道中选择动作，每轮由一次 LLM 调用决定做什么：

- **Channel 1 — Flags**：`-mllvm <pass的cost-model阈值参数>=<值>`，例如 `-mllvm --slp-threshold=-4`。只允许调整 cost model 的**阈值**，不允许绕过 cost model（`-force-vector-width`、`-disable-inlining` 等在黑名单里）。**注意**：这只约束了"合法性"，不代表数值语义不变 —— 降低阈值后被解锁的变换（例如 SLP 触发的浮点归约重排）仍然可能在浮点误差范围内改变结果。这一点此前完全没有被验证（见第 6.0 节，本次已修复）。
- **Channel 2 — Pragma hints**：在目标循环前插入 `#pragma clang loop vectorize(enable) vectorize_width(N)` / `unroll_count(N)` 等，转换为 `!llvm.loop` 元数据。规则里禁止对原地 stencil（循环携带依赖）和三角循环使用，但这个约束目前只写在 prompt 里让 LLM 自己遵守，还没有代码层面的静态检查。
- **Channel 3 — Source rewrite**：LLM 直接重写 kernel 函数体（分块 tiling、寄存器分块、循环交换、循环融合等）。必须通过两级数值等价性检查（见第 4 节）才会被接受；prompt 里明确禁止改变累加顺序、禁止给原地 stencil 加临时缓冲。

三个通道前两步各自独立测试（第 1 步强制 try_flags，第 2 步强制 rewrite_source），此后由 LLM/元规划器自由选择或组合（"compound" = source + flags 一起测）。

## 2. Agent 执行流程

```
baseline_ms  ← median(runtime(-O3(K)), 5 runs)          # 单次测量，问题见第6.1节
best_speedup ← 1.0 ; best_source ← None ; best_flags ← []
rollback_stack ← []

for step in 1 .. max_steps (默认 15，CLI --max-steps):
    forced_action ←
        "try_flags"       if step == 1
        "rewrite_source"  if step == 2
        反重复兜底：同一工具连续用了两次 → 强制换一个
        否则：从"计划序列"缓冲区取下一步（每3步调一次元规划 LLM）

    rollback_stack.push((best_source, best_flags, best_speedup))

    result ← run_agent_step(prompt(①..⑥+History), forced_action)   # 一次 LLM 调用
    history.record(step, result)

    if result.speedup < best_speedup × (1 − 20%):            # 灾难性退化回退
        (best_source, best_flags) ← rollback_stack.top()      # best_speedup 保持不变
                                                                # （history 不回退，LLM仍能看到失败记录）
    if result.action == "done": break
    if result.speedup > best_speedup:
        best_speedup, best_source, best_flags ← result.*

compound_speedup ← 若 source+flags 从未同测过，重测一次
confirmed_speedup ← 交替测量 baseline/best 各 N 次，配对比值取中位数（本次新增，见第6.1节）
report(best_speedup, compound_speedup, confirmed_speedup)
```

### 2.1 Prompt 的信息来源

每次 LLM 调用的 prompt 由 6 个带编号的段落 + 一个 History 段组成（token 预算 28000，超出时有明确的截断优先级）：

1. **硬件环境**：CPU 型号、SIMD 宽度、cache 大小。
2. **性能计数器 + 逆向推断**：`perf stat`（IPC、LLC miss率、分支miss率）+ VTune memory-bound%，并把测得的瓶颈映射到具体可调的 pass/参数（例如 `IPC=0.64, LLC-miss=19%` → memory-bound → 优先看 `licm-max-num-uses-traversed`、cache tiling）。
3. **Kernel 源码**：当前最优版本。
4. **LLVM IR**（O0，最多 80 行，截断时有明确标注，而不是静默截断）。
5. **Pass pipeline 图**：执行顺序 + **"起了作用" vs "no-op"** 的区分（通过 `src/ir_diff.py` 单独跑每个 pass 并 diff IR 前后得到，而不只是 `-debug-pass-manager` 报告的"跑过"）。
6. **富 Remarks**：精确到代码行/列、±2 行源码片段、向量化因子(VF)、交织因子(IC)、具体失败原因字符串（如 "cannot identify array bounds"），比 clang 默认的一行 remark 信息量大得多。
7. **History**：每一步的 action/speedup/error，三层黑名单（`failed_strategies`、`errored_strategies`、`strategy_speedups`），连续 4 步改进 <0.5% 时触发停滞警告。

（`optimize.py` 文件头注释里写的是"⑦ 段"，实际代码里 History 是单独一段拼在结构化 prompt 之后，⑦"策略记忆"被合并进了 History 段，不是独立小节——文档与实现有一点措辞出入，不影响功能。）

### 2.2 回退机制（Catastrophic Backoff）

`optimize.py` 第 3042-3073 行左右。每步开始前，系统保存 `(best_source, best_flags, best_speedup)` 快照。如果新候选比当前最优差超过 `catastrophic_slowdown_threshold`（默认 20%），系统状态（不是 LLM 看到的 history）会回退到上一个快照 —— LLM 仍然能在 history 里看到这次失败（用于学习规避），但搜索不会在退化的基础上继续累积。这是系统层的硬回退，区别于 LLM 自己基于 history 的"软"规避。设计初衷记录在 `docs/system_analysis.md` 第 5 节（这部分内容仍然成立，不属于过时内容）。

## 3. 正确性验证机制

`tune_source.py: compare_outputs()` —— 逐元素数值比较，`epsilon = 1e-4`；一个元素只有在**绝对误差和相对误差都超过 epsilon**（`diff > ε and rel > ε`，是 AND 不是更常见的 `atol OR rtol` 组合）时才算不一致；另外有数组尺寸不匹配、全零输出（隐藏逻辑 bug）、非数值错误字符串等专门检测。验证分两级数据规模：SMALL（快）和 STANDARD（相对 epsilon 放宽到 2×1e-4），STANDARD 超时会自动降级到 MINI。**但计时用的是 `LARGE_DATASET`，这个规模从来没有被做过正确性检查**（第 6.2 节）。

### 3.1 曾经存在、本次已修复的验证空洞

审计发现 **Channel 1（`try_flags`）此前完全没有调用任何正确性检查** —— 每个 `-mllvm` 候选和联合组合都只编译、计时、挑最快的，从未验证输出是否正确。这不是理论风险：`-slp-threshold` 这类参数直接控制 SLP 向量化是否触发，而 SLP 向量化会重排浮点归约的加法顺序 —— 这正是本项目自己在 source rewrite 通道里多次踩到的精度问题的同一种机制（见第 5.3 节失败模式）。也就是说，整个动作空间里大约三分之一的部分，此前对"更快但算错了"没有任何检测能力。

第二个相关空洞：即使在会做正确性检查的 `_eval_build_and_time`（`try_pragma`/`rewrite_source` 用）里，两级检查编译"opt"二进制时，也没有带上后面会一起用来计时的 `also_flags` —— 即真正被计时的"source+flags 组合"，和真正被验证的"仅 source"，其实不是同一个东西。

**已修复**：给 `_correctness_check()` 加了 `extra_flags` 参数，贯穿所有递归/降级分支和每个 `compile_c()` 调用；`_eval_build_and_time()` 现在把它自己的 `extra_flags` 传进两级检查里，堵上了 pragma/source+flags 的空洞；`try_flags` 分支现在会对最终胜出的 flags 组合跑同样的 SMALL→STANDARD 两级检查，验证失败会返回 `speedup=1.0` 和明确的 `"flags 数值验证失败"` 错误，而不是静默返回一个未经验证的加速比。

**仍未解决**：Phase A/B 搜索过程中被淘汰的候选（可能有几十个）依然不验证正确性，只验证最终胜出者。这是有意的成本/严格性权衡（每个候选都验证会让编译+运行开销成倍增加），但应该明确写成局限：一个"算错了但不是最快的"flag 值仍然不会被发现，只有"算错了而且最快"才会被现在的检查拦住。

## 4. 实验数据

仓库里有两批数据，**不能混用**：

- **`POLYBENCH_REPORT.md`**（2026-06-23 → 2026-06-24 生成）：旧版"1 轮 param + 8 轮 source"流程，早于当前 `optimize.py` 里的统一 agent 循环架构。定性的失败模式分析（第 5.3 节）仍然成立，可以引用；但其架构描述本身已过时。
- **`runs/2026-06-28_*` … `runs/2026-06-30_*`**：真正的 `optimize.py` agent 模式跑出来的结果（`max_steps=10`），每个都有 `results.json`、`outputs/<name>_agent_results.json`、完整的 `llm_calls.jsonl`。这是当前架构下的权威数据来源 —— 但同样要注意，这批数字全部是**单次试验、未经二次确认**的 `best_speedup`（第 6.1 节）。
- **`README.md` 里 2026-06-16 的遗传算法实验**（jacobi-2d 21.6%、atax 41.9%、heat-3d 31.9%、2mm 11.4%）是更早期、只搜索 GCC flags 的原型，已经被上面两批 LLVM/agent 结果取代，只作历史参考，不应作为当前系统的代表性数据。

### 4.1 当前 agent 架构结果（runs/2026-06-28 → 2026-06-30，max_steps=10）

| Benchmark | best_speedup | steps_taken |
|---|---:|---:|
| gramschmidt | 22.67× | 10 |
| trmm | 15.69× | 10 |
| doitgen | 8.22× | 10 |
| syrk | 8.09× | 10 |
| symm | 7.25× | 10 |
| syr2k | 6.09× | 10 |
| 3mm | 5.27× | 10 |
| 2mm | 4.05× | 10 |
| bicg | 3.81× | 10 |
| mvt | 2.68× | 10 |
| gemver | 2.47× | 10 |
| deriche | 1.83× | 10 |
| covariance | 1.42× | 10 |
| heat-3d | 1.58× | 10 |
| gesummv | 1.58× | 10 |
| lu | 1.41× | 10 |
| cholesky | 1.20× | 10 |
| trisolv | 1.27× | 10 |
| nussinov | 1.25× | 10 |
| floyd-warshall | 1.14× | 10 |
| ludcmp | 1.13× | 10 |
| adi | 1.13× | 10 |
| gemm | 1.06× | 10 |
| correlation | 1.08× | 10 |
| atax | 1.08× | 10 |
| fdtd-2d | 1.05× | 10 |
| durbin | 1.0× | 2（提前 "done"）|
| jacobi-1d | 1.0× | 3（提前 "done"）|
| jacobi-2d | 1.0–1.10× | 5–10（两次尝试，第二次重跑测到 1.103×）|
| seidel-2d | 1.0× | 2（提前 "done"）|

**值得注意的差异**：`durbin`、`jacobi-1d`、`jacobi-2d`、`seidel-2d` 在当前 agent 下提前终止、没有改进（LLM 在 2-5 步后就主动输出 `done`）——这几个正好是第 5.3 节里"循环携带依赖/O3已充分向量化"类失败模式的对象，说明当前 agent 至少能"诚实地"识别自己没有更多可做的，虽然旧版流程靠"强制跑满 8 轮 source"偶尔能硬找到一点小提升（如 jacobi-2d 1.114×）。

### 4.2 旧版流程结果（POLYBENCH_REPORT.md，2026-06-23→24，约16.5小时，30个benchmark，3并发，1轮param+8轮source）

**Geomean 最终加速比: 3.187×**

| 分类 | Benchmark | Baseline (ms) | Param-only | Source-only | **最终最优** |
|---|---|---:|---:|---:|---:|
| >5× | gramschmidt | 10964 | 1.022× | 16.727× | **16.727×** |
| >5× | correlation | 5851 | 1.001× | 16.454× | **16.454×** |
| >5× | trmm | 2521 | 1.014× | 13.032× | **13.032×** |
| >5× | nussinov | 4872 | 1.028× | 12.533× | **12.533×** |
| >5× | doitgen | 588 | 1.047× | 8.110× | **8.110×** |
| >5× | syrk | 1624 | 1.035× | 7.853× | **7.853×** |
| >5× | 2mm | 2143 | 1.030× | 7.358× | **7.358×** |
| >5× | 3mm | 3494 | 1.100× | 7.299× | **7.299×** |
| >5× | symm | 2971 | 1.116× | 6.093× | **6.093×** |
| >5× | syr2k | 4719 | 1.063× | 5.537× | **5.537×** |
| 2–5× | ludcmp | 5347 | 1.126× | 4.525× | **4.525×** |
| 2–5× | mvt | 5.45 | — | 4.143× | **4.143×** |
| 2–5× | durbin | 3.15 | 1.100× | 3.894× | **3.894×** |
| 2–5× | bicg | 10.64 | 1.031× | 3.713× | **3.713×** |
| 2–5× | gemver | 26.62 | 1.021× | 3.499× | **3.499×** |
| 2–5× | deriche | 311 | 1.122× | 2.221× | **2.221×** |
| 2–5× | atax | 7.00 | 1.083× | 2.026× | **2.026×** |
| <2× | fdtd-2d | 2253 | **1.393×** | 1.850× | 1.850× |
| <2× | trisolv | 3.69 | **1.202×** | 1.818× | 1.818× |
| <2× | gesummv | 4.78 | 1.055× | 1.686× | 1.686× |
| <2× | gemm | 423 | 1.031× | 1.352× | 1.352× |
| <2× | floyd-warshall | 16061 | **1.094×** | 1.182× | 1.182× |
| param-only | heat-3d | 2109 | **1.281×** | 1.023× | 1.281× |
| param-only | lu | 5726 | **1.151×** | ✗ 8/8失败 | 1.151× |
| param-only | covariance | 6293 | **1.091×** | ✗ 7/8失败 | 1.091× |
| param-only | jacobi-2d | 1653 | **1.114×** | 1.002× | 1.114× |
| param-only | jacobi-1d | 0.47 | 1.071× | 1.040× | 1.071× |
| param-only | cholesky | 1449 | 1.012× | ✗ 8/8失败 | 1.012× |
| param-only | adi | 9855 | 1.016× | 1.002× | 1.016× |
| param-only | seidel-2d | 20639 | 1.027× | 1.017× | 1.027× |

### 4.3 失败模式分类（定性，来自逐轮日志）

| 失败模式 | 涉及 benchmark | 根因 |
|---|---|---|
| 循环携带依赖盲区 | `lu`（8/8轮）、`cholesky`（8/8）、`seidel-2d`（6/8）| LLM 反复把要求严格顺序求值的原地三角求解 / Gauss-Seidel 更新并行化/向量化；不能可靠识别 `A[i][k]` 在同一遍里先被更新后又被读取。|
| 浮点重排序精度违规 | `covariance`、`gesummv`、`syr2k`（前6轮）、`mvt`（第7轮）| 循环重排/向量化改变浮点累加顺序；误差很小（`rel ≈ 1.5–4×10⁻⁴`）但超过 `ε=1e-4`。|
| 无效 pragma / 不可移植语法 | `bicg`、`trmm`、`doitgen`、`3mm` | LLM 用了 GCC/ICC 专属写法（如 `#pragma ivdep`），Clang 不认；偶尔直接把解释性文字写进 `.c` 文件。|
| 分块导致退化 | `adi`、`heat-3d`、`jacobi-1d/2d`、`floyd-warshall` | 访存模式规则的 stencil/solver 已被 `-O3` 充分自动向量化；LLM 手写的分块只增加循环开销，没有对应的 cache 局部性收益（heat-3d 最差跌到 0.45×）。|
| 访存模式突破 | `correlation`、`gramschmidt`、`trmm`、`nussinov` | 列主序 → 行主序改写消除了主要 cache miss 来源；这是整次实验里收益最高的一类变换。|

## 5. 系统还存在哪些问题（按严重程度排列）

### 5.0 Channel 1（try_flags）此前完全没做正确性验证 —— 【本次已修复】

见第 3.1 节详细说明。这是审计中发现的最严重问题：约三分之一的动作空间此前对"更快但算错"零检测能力。已修复（给 `_correctness_check` 加 `extra_flags` 参数并在 `try_flags` 里接入两级检查）。

### 5.1 "最优结果"从未做过二次确认测量 —— 【本次已修复】

**问题**：每个 benchmark 报告的 `best_speedup` 就是该轮次单次测量（5次运行取中位数）的原始结果，测量时机是多分钟 agent 运行过程中随便哪一步。从未被独立复测过。再加上是"最多 15 步里挑最好的一步"，这是典型的**多重比较 / 挑数**问题：报告的数字被噪声系统性高估，而且步数越多偏差越大。`configs/config.yaml` 里其实定义了 `runtime.confirm_speedup_threshold: 5.0`（"speedup≥5%时复测3次过滤假阳性"），但这个字段是**死代码**，`optimize.py` 里从没被引用过。

**已修复**（`optimize.py` 新增 `confirm_result()` + `_single_shot_ms()`，接入运行结束时的最终报告）：agent 循环结束后，重新编译 baseline 和胜出的 `(source, flags)` 组合，做**交替**单次测量（`base, best, base, best, …`，`args.runs` 对，默认 5 对），确认后的加速比取**配对比值的中位数**，而不是"分钟级时间差下分别测出的 median(base)/median(best)"。交替测量能抵消单次前置 baseline 无法控制的慢性漂移（温控降频、其他进程负载）；配对比值取中位数是处理这类匹配样本噪声的标准做法。结果 JSON 现在带有 `confirmed_speedup`、比值分布的 IQR、以及两侧的变异系数（`base_stdev_pct`、`best_stdev_pct`），让噪声水平可见，而不是一个孤立的点估计。

**仍未解决**：这依然只是每个 benchmark 测一次程序运行，不是重复整个 agent 搜索过程（见第 5.4 节）。

### 5.2 计时用的数据规模从未做过正确性检查

正确性在 `SMALL_DATASET`/`STANDARD_DATASET` 验证；实际报告的数字（`compound_speedup`、`confirmed_speedup`）是在 `LARGE_DATASET` 上测的。一个在 SMALL/STANDARD 下数值正确、但有尺寸相关 bug（比如只在更大分块数下才触发的越界）的重写，可能悄悄产出一个"更快但算错了"的 LARGE_DATASET 二进制。**建议**：在最终接受候选前，加一道 LARGE_DATASET 的正确性检查（哪怕只是跟缓存的 `-O3` 参考输出做哈希比对）。

### 5.3 没有留出验证集 / 没有 train-test 划分

每个 benchmark 报告的数字都是"在同一个 benchmark 上跑 8~10 轮里最好的一次"，即搜索和评估用的是同一个实例，没有独立复现。没有跨 benchmark 的泛化测试（比如在 20 个 benchmark 上调 prompt/策略设计，在留出的 10 个上报告结果）。如果要论证这是一个*通用*的 beyond-O3 搜索方法（不是逐个 kernel 手调），这是最重要的、需要补上的一环。

### 5.4 每个 benchmark 只跑一次 —— agent 本身的搜索过程没有方差数据

LLM 调用温度 `temperature=0.6`，没有固定 seed，agent 自己的动作序列本身就是随机的。`POLYBENCH_REPORT.md` 里每个 benchmark 只跑了一次完整的 agent 搜索。要达到可发表的严谨程度，需要每个 benchmark 至少独立跑 3-5 次完整 agent 搜索，报告中位数和离散范围（第 5.1 节已经把这种严谨性用在"计时测量"上了，还需要同样用在"搜索过程"本身），否则一次幸运（或不幸）的 rollout 和方法真实的期望表现是无法区分的。

### 5.5 没有 `-O3` 之外的对照

对照组只有 `{-O3}`。要让人信服"这是 LLM 的贡献"而不是"随便一个 15 步搜索都能找到"，至少需要以下之一：在同样的 flag/pragma/rewrite 动作空间上做随机搜索、贝叶斯优化（类 OpenTuner）、或者在同等编译/测量预算下和 CompilerGym/BOCA/AutoPhase 这类 RL 搜索方法比较。

### 5.6 没有消融实验

`docs/implementation_report_for_advisor.md` 第 10 节已经对（现已废弃的）MCTS 版本提出过这个问题，同样适用于现在的架构：没有实验单独隔离出 (a) 富 remarks 段 vs 普通 remarks 的贡献，(b) 回退机制开/关，(c) 元规划动作序列缓冲 vs LLM 自由选择，(d) 三个通道各自单独在"compound 获胜"的 benchmark 上的贡献。没有这些实验，类似"富 remarks 段是核心创新点"（`docs/system_analysis.md` 第4.2节的说法）这样的论断目前只是断言，没有被证明。

### 5.7 没有统计显著性检验

第 4.1/4.2 节的表格只有加速比点估计，没有置信区间，也没有配对显著性检验（比如跨 benchmark 做 Wilcoxon signed-rank test，检验"相对于不做任何改动"的零假设）——这在 HPC/编译优化类顶会（CGO/PLDI/ASPLOS）对这种噪声较大的微基准计时结果是标配要求。

### 5.8 两套互不统一的"baseline"

全局 baseline（`baseline_time`，运行开始时测一次）用于 `try_flags`/`try_pragma` 的加速比计算；但 `_eval_build_and_time`（`rewrite_source` 用）**每次调用都重新编译并重新测一个自己的参考二进制**（`ref_time_bin`），不复用 `baseline_time`。两者名义上都是"`-O3` baseline"，但在一次可能长达数小时的运行里是在不同时间点测的 —— 也就是说同一次运行里，`rewrite_source` 步骤和 `try_flags` 步骤报告的加速比，分母其实不是同一个数。这会加重第 5.1/5.4 节提到的漂移问题，建议统一成一个周期性刷新的共享 baseline。

### 5.9 容差语义 & config/代码不同步

`compare_outputs()` 只有在**绝对误差和相对误差都**超过 epsilon 时才判定不一致（AND，不是更常见的 `atol OR rtol` 组合）。这比看起来更宽松：一个相对于巨大参考值有大绝对误差（但相对误差很小）的元素，或者一个相对于接近零的参考值有大相对误差（但绝对误差很小）的元素，都可能被放过。对 PolyBench 这种数值量级比较规整的数组问题不大，但应该明确写成"有意为之的选择"，而不是隐含在代码里没人知道。

另外，`configs/config.yaml` 里写着 `runtime.measurement_repeat: 9` 和 `runtime.confirm_speedup_threshold: 5.0`，当前 `optimize.py` 都不读这两个字段 —— 实际生效的是 CLI 默认 `--runs 5`，而"复测确认"在本次修复之前完全不存在。一份"配置文件"如果悄悄不再支配它声称配置的代码，本身就是一个可复现性问题（详见第 8 节）。

### 5.10 工具链版本较旧

整条 pipeline 固定在 **LLVM/Clang 11**（2020年发布）。作为"针对某个固定 baseline 编译器做优化"是可以自洽的，但如果要写成论文，应该明确说明：这里发现的 beyond-O3 提升空间，在更新的 `-O3`（比如 LLVM 17+）上是否依然存在 —— 部分被利用的 cost model 保守性可能上游已经修掉了。

## 6. 本次代码修复总结

在 `optimize.py` 中做了两处修复：

1. **`_correctness_check()` 新增 `extra_flags` 参数**，贯穿所有递归/降级调用路径和每个 `compile_c()`。`_eval_build_and_time()` 的两级检查现在会带上 `also_flags` 一起验证。`try_flags` 分支新增了对最终胜出 flags 组合的两级 SMALL→STANDARD 正确性验证（对应第 5.0 节）。
2. **新增 `confirm_result()` / `_single_shot_ms()`**，在 agent 循环结束、报告最终结果前，对 baseline 和胜出候选做交替单次测量，取配对比值中位数作为 `confirmed_speedup`，并附带 IQR 和变异系数写入结果 JSON（对应第 5.1 节）。

两处都做过 `python3 -m py_compile optimize.py` 语法检查，未做端到端 PolyBench 全量重跑验证（跑一次 30-benchmark 套件在这台机器上需要数小时，超出本次审计的时间预算 —— 建议作为下一步执行）。

## 7. 待办事项（按优先级）

1. 用修复后的代码重跑全部 benchmark，用 `confirmed_speedup` + IQR + 每个通道现在都会给出的正确性验证结果，重新发布第 4 节的表格 —— 当前表格里的数字都早于这两处修复，应视为临时数据。
2. 每个 benchmark 至少独立跑 3-5 次完整 agent 搜索，报告中位数和范围（第 5.4 节）。
3. 统一两套 baseline 测量路径，改成一个周期性刷新的共享 baseline（第 5.8 节）。
4. 加一个非 LLM 的对照组（同样的三通道动作空间、同样的步数/编译预算下做随机搜索），把 LLM 的贡献单独隔离出来（第 5.5 节）。
5. 给最终候选加一道 LARGE_DATASET 正确性检查（第 5.2 节）。
6. 清理或明确隔离过时的 MCTS 文档和 config 字段（第 8 节，第 5.9 节）——大约 30 分钟工作量，能消除一个实实在在的可复现性隐患。
7. 设计并跑第 5.6 节列的消融实验，优先做：富 remarks 开/关、回退机制开/关。
8. 待 prompt/策略设计稳定后，划出留出 benchmark 集合（第 5.3 节），支撑"方法具有通用性"的论断。

## 8. 文档 / 代码不一致问题

`docs/aim.md` 和 `docs/implementation_report_for_advisor.md` 描述的是一套 MCTS 为核心的架构（`run.py`、`mcts_core.py`、`mcts_expansion.py`、`reward_functions.py`、`pass_concept_mapper.py`、`dynamic_pass_extractor.py`），**这些源文件在仓库里已经不存在**；当前实际运行的是第 2 节描述的"每步一次 LLM 调用"的 agent 循环。`configs/config.yaml` 里还留着一大块 `mcts:` 配置（PUCT 常数、reward 校准注释等），当前 agent 循环完全不读这些字段。这不只是美观问题：如果有人（审阅人，或者未来的你自己）只看 `configs/config.yaml` 去调参数复现结果，调的参数对实际代码路径毫无影响。**建议**：要么 (a) 删掉死掉的 `mcts:`/`rollout:`/`knowledge_base:` 配置段和这两份过时文档，要么 (b) 明确挪到 `legacy_mcts_prototype/` 之类的目录下打上标签，让当前唯一的事实来源（`optimize.py` + `configs/config.yaml` 里真正被读取的 `runtime:`/`llm:`/`compiler:` 段）没有歧义。

---

*信息来源：`optimize.py`、`tune_param.py`、`tune_source.py`、`src/*.py`、`configs/config.yaml`、`POLYBENCH_REPORT.md`、`docs/system_analysis.md`、`docs/implementation_report_for_advisor.md`、`docs/aim.md`、`outputs/atax_agent_results.json`、`runs/2026-06-28_*` ~ `runs/2026-06-30_*`。*
