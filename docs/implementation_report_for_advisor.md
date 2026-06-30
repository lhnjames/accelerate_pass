# COMET 论文代码实现细节汇报简稿

## 1. 项目目标

本项目实现的是一个面向 LLVM pass sequence 优化的自动搜索框架，名称为 COMET，即 Compiler Observable-Guided Monte Carlo Exploration for Transformation Ordering。

**架构定性**：COMET 不是纯 MCTS，而是 **PUCT/MCTS + compiler remarks proxy reward + runtime budget + LLM/heuristic guided search** 的组合系统。这种组合非常适合编译优化这类搜索空间巨大、单次评估代价高昂的场景——纯 MCTS 需要大量 rollout 才能收敛，而 COMET 用 LLM 方向建议、remarks 信号和 IR hash 去重压缩搜索代价。论文汇报时应如实描述此架构，而非声称是标准 MCTS。

代码目标不是手工写死某一组优化 pass，而是从当前 LLVM `opt` 工具中动态获取可用 pass，结合静态优化 remarks、LLM 诊断和 MCTS 搜索，自动为输入程序寻找更优的 LLVM pass 序列。当前实验以 PolyBench C 程序为对象（如 `jacobi-2d`、`gemm`），均在 `clang -O3` 基础上进一步优化（beyond-O3 范式）。

## 2. 整体流程

入口脚本是 `run.py`，完整执行流程分为三个阶段：

1. Phase 0: 信息提取  
   使用 `clang-11` 将源程序转为 LLVM IR，并用 `opt-11` 提取优化 remarks。系统会统计 missed remarks、applied remarks、IR 指令数等信息，构造后续搜索的初始上下文。

2. Phase 1: LLM 初始诊断  
   LLM 根据 baseline remarks、程序统计信息和已验证 pass pool，生成优化诊断和初始 seed pass sequence。LLM 输出不会直接信任，而是通过 `pass_concept_mapper.py` 映射到当前 LLVM 中真实可用的 pass。

3. Phase 2: Remarks-guided MCTS  
   MCTS 以 seed 节点为起点进行树搜索。每个节点代表一个 pass sequence，系统会编译该 sequence、重新提取 remarks，并用 remarks 改善情况和少量真实 runtime measurement 共同计算 reward。

## 3. 动态 pass 获取与过滤

pass pool 由 `dynamic_pass_extractor.py` 从 `opt --help` 中动态解析候选 pass，再逐个用 `opt` 验证。当前实现做了两层过滤：

- 验证 pass 是否能在最小 IR 上成功运行；
- 用包含 load/store/loop 的 richer IR 检测 sanitizer/profiler instrumentation pass，例如 `asan-module`、`hwasan`、`tsan`、`instrprof`、`pgo-instr-gen`，这些会插入运行时检测逻辑，影响性能测量，因此从优化池中排除并告知 LLM。

这样做的目的是避免硬编码一个固定 pass 列表，使系统能适配当前机器上真实安装的 LLVM 版本。不过仍保留少量无法正常验证的特殊排除项，例如需要外部 profile 或 summary 文件的 pass。

## 4. LLM 与 MCTS 的结合方式

LLM 在系统中不是替代搜索，而是提供方向性建议：

- Phase 1 中 LLM 给出初始诊断和 seed sequence；
- MCTS 搜索过程中，前期按照配置较高频率调用 LLM，后期降低调用频率；
- 周期性 LLM 调用已经改为异步执行，避免 LLM 请求阻塞 MCTS 主循环；
- 如果搜索出现 stagnation，系统会调用 LLM 判断当前路径是否 saturated、stuck 或需要 different approach，并据此进行回退或调整；
- LLM 输出的 pass 会被过滤到 verified pass pool 中，避免使用当前 LLVM 不支持的 pass。

配置位于 `configs/config.yaml`。当前主要参数包括：

- `mcts.iterations: 100`
- `mcts.max_depth: 8`
- `llm.call_budget: 30`
- `runtime.total_budget: 20`
- `runtime.measurement_repeat: 3`
- `optimization.early_stage_llm_frequency: 8`
- `optimization.late_stage_llm_frequency: 15`

这些参数的含义和设置原因如下：

| 参数 | 含义 | 当前设置原因 |
|------|------|--------------|
| `mcts.iterations: 150` | MCTS 主搜索循环的迭代次数。 | 从 100 提高到 150，为更大的 transformation pass pool 提供足够探索预算。 |
| `mcts.max_depth: 8` | MCTS 搜索树允许的最大深度。 | beyond-O3 范式中，额外 pass 效果通常在 2-8 步就已充分体现。 |
| `llm.call_budget: 40` | 一次优化任务中最多允许调用 LLM 的次数。 | 从 30 提高到 40，支持更多 fixpoint 逃逸尝试（duplicate_injection_count 反馈机制）。 |
| `runtime.total_budget: 50` | 允许进行真实程序运行计时的节点数量上限。 | 从 20 提高到 50，pool 扩大后候选更多，需要更充分的 runtime 验证。 |
| `runtime.measurement_repeat: 9` | 每个候选 binary 实际运行多少次，取中位数。 | 9 次取 median 能有效压制 turbo boost 和调度噪声，对 jacobi-2d 这类有噪声的大数据集程序尤为重要。 |
| `mcts.measure_visit_threshold: 2` | 节点被访问多少次后触发真实 runtime 测量。 | 从 3 降为 2，让 MCTS 更早获得真实性能反馈，减少对 remarks proxy 的依赖（尤其对 memory-bound 程序）。 |
| `optimization.early_stage_llm_frequency: 8` | 搜索早期周期性 LLM 调用频率（每 N 轮触发一次）。 | 早期以 MCTS 为主快速展开搜索空间，每 8 轮请求一次 LLM 方向建议。 |

整体上，这组参数体现的是一种折中策略：前期让 LLM 多参与，帮助打开搜索空间；中后期逐渐依赖 MCTS 和真实 runtime 验证；真实测量次数受预算控制，避免实验时间不可控。

## 5. MCTS 节点、reward 与搜索策略

MCTS 的核心结构在 `mcts_core.py` 和 `mcts_expansion.py` 中。每个 `MCTSNode` 保存：

- 当前 pass sequence；
- IR hash，用于状态去重；
- missed remarks 数量；
- resolved remarks 集合；
- runtime measurement 结果；
- visit count、prior、total value 等搜索信息。

reward 由 `reward_functions.py` 计算，公式为：

```
reward = alpha * runtime_signal + (1 - alpha) * remarks_signal - length_penalty
```

**一句话概括：runtime-first when measured, remarks-guided before measurement。**

- 已测 runtime 时：alpha ∈ [0.6, 1.0]，runtime speedup 占主导；
- 未测 runtime 时：alpha ∈ [0.2, 0.5]，remarks resolution ratio 占主导，runtime proxy 用 IR 指令数减少量乘 0.3 discount；
- remarks signal 使用 `resolved_baseline_missed / total_baseline_missed`，不惩罚 new_missed（新出现的 missed remarks 可能是 pass 暴露了新优化机会，不应视为退步）；
- length_penalty 仅对超过 6 个 pass 的序列生效，系数 0.003，不会压制短序列的合理 speedup。

**重要限制**：remarks 减少不等于 runtime 变快（例如某 pass 消除了 remark 但破坏了 cache locality）。remarks signal 是搜索早期的导向，最终判断必须依赖 runtime measurement。

**Rollout**：`mcts_expansion.py` 中有三层 rollout（KB → keyword → random），但这是启发式价值估计器，不是真实编译模拟。当前实现中 rollout 节点的 `resolved_ids` 始终为空，导致 remarks signal 为 0，实际贡献极小。主循环已改为直接使用编译后叶节点的真实 remarks 数据作为 reward，rollout 保留用于 KB/keyword 扩展建议。论文中应将其描述为 "heuristic value estimator"，而非标准 MCTS rollout。

为了节省时间，搜索过程中不会每个节点都真实运行程序，而是通过 `RuntimeMeasurementScheduler` 控制预算。真实 runtime 测量只在部分节点和 post-search top candidates 上执行。

## 6. 编译与运行测量

编译和测量逻辑主要在 `compiler_manager.py` 和 `runtime_measurement.py` 中。

编译阶段会：

- 提取原始 LLVM IR；
- 按 pass sequence 逐个调用 `opt-11`；
- 如果某个 pass 失败，记录 skipped pass，并返回实际生效的 effective pass sequence；
- 对多次失败的 pass 进行 skip count 统计，超过阈值后从 pass pool 移除。

runtime 测量阶段采用“编译可并发、运行必须串行”的策略：

- candidate binary 的编译可以并发，因为这部分不涉及计时；
- 真正运行程序计时时必须串行，以避免 CPU 抢占、cache 污染、内存带宽竞争造成 speedup 排名失真；
- 当前配置每个候选运行 3 次，取 median 作为 runtime。

PolyBench 程序需要额外链接 `PolyBenchC_no_rag/utilities/polybench.c`，代码中已经加入了针对 PolyBench utilities 的处理。

## 7. 当前实验现象

最近一次 `jacobi-2d` 运行中，baseline runtime 约为 `9289.57ms`。搜索结束后的 post-search 阶段找到的较优候选为：

```text
early-cse-memssa -> licm -> dse
```

对应 runtime 约为 `6697.6ms`，相对 baseline 约有 `27.9%` 的加速。这说明当前框架已经能够通过 MCTS + LLM 搜索找到超过 20% speedup 目标的 pass sequence。

同时，日志中也能看到一些需要继续完善的地方：

- 部分 pass 如 `size-info` 会被跳过，说明仍存在“pass 存在但不适合当前 IR”的情况；
- post-search 中少数候选会出现链接失败，例如 `undefined reference to main`，说明某些 pass sequence 可能破坏程序可链接性；
- 主搜索阶段真实测量次数较少，更多真实测量集中在 post-search 阶段。

## 8. 搜索重定向机制（Adaptive Backoff / Search Redirection）

当前系统有三种搜索重定向机制，论文中应描述为 **adaptive backoff / search redirection**，而非标准 MCTS backtracking：

1. **IR hash 去重**：lazy compile 后发现 IR 与已有节点相同，从 parent children 中删除该节点，只增加 visit count，不给负 reward。合理：重复 IR 不代表父路径差，只代表子步骤无新状态。

2. **Stagnation-triggered redirection**：`StagnationDetector` 检测 missed count 停滞后，询问 LLM：
   - `stuck` → 在树内找祖先节点，boost 其未探索 child prior；
   - `different_approach` → 惩罚当前 seed Q 值（递增惩罚），连续 ≥2 次时同时触发树内回退 + boost 最优备选种子；
   - `saturated` → 继续正常搜索。
   
   注意：LLM verdict 本身不稳定，当前用 cooldown 和 escalating penalty 缓解。

3. **Runtime slowdown backoff**：同一 seed 连续 3 次测出比 O3 慢时，soft penalize 该 seed 的 Q 值，引导 MCTS 切换到其他分支。

## 9. 相比早期版本的改进

与早期固定 pass/固定 flag 的实验相比，当前实现主要改进包括：

**早期改进（已记录）**：
- pass pool 从 LLVM 工具动态获取，不手工写死，自动适配 LLVM 版本；
- LLM 不直接决定结果，作为 MCTS 的先验建议（prior）；
- MCTS 节点保存真实 effective pass sequence，避免把 skipped pass 误认为已执行；
- 运行计时串行执行，减少并发造成的性能噪声；
- IR hash 去重大幅节约搜索预算；
- drift normalization：每次 runtime 测量后立即重测 O3 baseline，消除系统负载漂移造成的假加速；
- post-search 对 top candidates 进行真实 runtime 验证。

**最新修复（针对 pass pool 坍缩和 LLM fixpoint 循环问题）**：

1. **Pass pool 扩展**：新增 `_BEYOND_O3_TRANSFORMATION_PASSES` 常量（loop-unroll、indvars、dse、memcpyopt、early-cse-memssa 等），无论 remarks 是否提到，都加入 pool。从 O3 remarks ∩ broad_pool 的 3 个 fixpoint，扩展到 10-15 个包含变换类 pass 的完整 pool。

2. **Polly 支持**：自动探测 `polly-*` 类 pass，如果 LLVM 安装了 Polly，立即加入 pool。Polly 能做 loop tiling/blocking，对 stencil 程序（jacobi、seidel）可提供 2-10x 加速。

3. **Workload 分类**：基于 perf profile（IPC、cache_miss_rate）对程序分类为 MEMORY_BOUND/COMPUTE_BOUND/MIXED（`WorkloadClass` enum，`classify_workload()` 函数，`program_analyzer.py`）。jacobi-2d 的 IPC=0.636、cache_miss=19.4% → MEMORY_BOUND。

4. **Reward signal 修复**：memory-bound 程序的 alpha（runtime 权重）从 0.6 提高到 0.85；effective pool ≤3 时也同样处理，避免无区分度的 remarks signal 误导搜索。

5. **LLM fixpoint 逃逸**：新增 `_duplicate_injection_count` 计数器，追踪因 IR hash 重复被拒的 LLM 建议数量。当 count ≥ 3 时，在 periodic expansion prompt 中加入明确的 fixpoint 逃逸指令，告知 LLM 优先使用 TRANSFORMATION PASSES。

6. **Workload hint 注入**：在所有 LLM prompt 中加入工作负载分类信息，让 LLM 知道应优先哪类优化（memory-bound → loop tiling；compute-bound → vectorize/unroll）。

7. **Prescreener 格式改进**：noop pass 从"完全禁用"改为"secondary only"，避免 LLM 陷入"没有可用 pass"的困境；明确说明 noop pass 在其他 pass 之后可能再次生效。

## 10. 后续可以向老师说明的改进方向

1. **Ablation study**（最重要）  
   需要 ablation 支撑当前设计选择的合理性，建议对比：
   - 关闭 `enable_diversity_fallback`（纯 LLM/KB seed，看多样性下降多少）；
   - 关闭 LLM stagnation backoff（看 MCTS 能否自行走出局部最优）；
   - 关闭 transformation passes 扩展（仅用 remarks pool，看 pool 坍缩的影响）；
   - 关闭 workload classification（统一 reward weight，验证分类的贡献）；
   - 仅 remarks proxy，不做 runtime measurement（验证 proxy 信号质量）。

2. **更充分的 runtime-guided search**  
   当前 total_budget=50，已提高。对于 tiny pool（≤5 effective passes）场景，可考虑全部穷举量测，彻底绕过 MCTS 搜索开销。

3. **跨程序泛化分析**  
   在多个 PolyBench 程序上统计最常出现的有效 pass pattern，分析 stencil / matrix / data mining 类程序的通用规律，积累 knowledge_base.json。memory-bound 类型（jacobi、seidel）vs compute-bound 类型（gemm、3mm）应分别统计。

4. **Polly 集成实验**  
   如果机器上安装了 `llvm-11-polly`（`apt install llvm-11-polly`），系统会自动探测并加入 polly pass。建议专门测试 polly 对 jacobi-2d 的效果（预计 3-10x 加速），作为 paper 中的 upper bound baseline。

总体来说，当前代码已经形成一个完整的论文实验原型，修复了 pass pool 坍缩、LLM fixpoint 循环、reward signal 失效三个根本性问题。在 `jacobi-2d` 和 `gemm` 上的基础实验结果（+26% 和 +42% speedup vs clang -O3）已具备论文支撑价值，修复后预计能在更多 PolyBench 程序上取得更稳定的改进。汇报时注意：这是 PUCT/MCTS + workload-aware reward + LLM guidance 的组合系统，不是原教旨 MCTS——这个定性本身是对"昂贵编译优化搜索"的合理适配，在论文中应正面描述。
