# COMET 系统分析文档

**版本**: 2026-06-27  
**范围**: LLVM Pass 图可视化、参数注入方式、内存管理、Prompt 有效性、回退机制、无数据集优化策略

---

## Section 1: LLVM Pass Pipeline 可视化

### 1.1 实现机制

`src/pass_graph.py` 实现完整的 pass pipeline 提取与 DOT 图渲染。

**提取流程**:
```bash
# opt-11 运行 O3 pipeline，输出 debug 信息到 stderr
opt-11 -passes=default<O3> -debug-pass-manager -disable-output kernel.ll
```

使用 `-disable-output` 的原因：只需观察 pass 执行顺序，不需要生成优化后 IR，避免写磁盘开销。

**解析 stderr 中的 pass 事件**:
```
Running pass: SLPVectorizerPass on kernel_gemm
Running pass: LoopVectorizePass on kernel_gemm
Running pass: LoopUnrollPass on loop
```
通过 `kernel_name in target` 过滤，只保留作用于目标 kernel 的 pass 事件。

### 1.2 图结构

**节点** (`build_graph()`):
- `name`: pass class name（来自 LLVM 内部）
- `label`: 人类可读短标签（如 `SLP-Vect`）
- `category`: vectorize / loop / memory / scalar / inline / other
- `color`: 按 category 分配颜色
- `runs`: 该 pass 在 kernel 上运行次数（显示为 `×N`）
- `missed`: missed remarks 数量（来自 `-Rpass-missed=.*`）
- `passed`: applied remarks 数量
- `ir_changed`: **新增** — pass 是否实际修改了 IR（来自 `src/ir_diff.py`）
- `ir_delta_vec`: **新增** — IR 向量操作变化量
- `ir_delta_instr`: **新增** — IR 总指令变化量

**边**:
- 按 pass 第一次出现顺序排列（`rankdir=LR`）
- 去重：相同前驱→后继对只保留一条边

### 1.3 DOT 图颜色规范

| 颜色/样式 | 含义 |
|----------|------|
| 蓝色填充 | vectorize 类 pass（SLP-Vect, Loop-Vect）|
| 绿色填充 | loop 类 pass（Unroll, Rotate, Interchange）|
| 黄色填充 | memory 类 pass（LICM, DSE, MemCpyOpt）|
| 橙色填充 | scalar 类 pass（GVN, InstCombine, CSE）|
| 紫色填充 | inline 类 pass（Inliner）|
| **红色边框** | 有 missed remarks（明确的优化机会被拒绝）|
| **绿色边框** ✓ | **新**: pass 确实修改了 IR（`ir_changed=True`）|
| **灰色填充** | **新**: pass 运行了但 IR 没有变化（no-op）|

### 1.4 关键区别："运行了" vs "起了作用"

以前系统只知道哪些 pass 运行了（来自 `-debug-pass-manager`）。现在通过 `src/ir_diff.py` 的独立运行测试，区分：

- **FIRED（绿框）**: `opt -passes=slp-vectorizer ir.ll` 前后 IR 有变化（向量操作数、指令数改变）
- **no-op（灰填）**: 运行了但 IR 完全没变 → LLM 应降低对该 pass 的调参优先级
- **skipped（原色）**: 由于 analysis 依赖无法单独运行（如 InlinerPass）→ 状态未知

### 1.5 输出文件

```
outputs/<kernel>_pass_graph.dot   # graphviz DOT 源文件
outputs/<kernel>_pass_graph.png   # 渲染 PNG（需 graphviz 安装）
outputs/<kernel>_pass_graph.svg   # 渲染 SVG
```

---

## Section 2: LLVM 参数注入方式（四通道）

### Channel 1: `-mllvm` Cost-Model 阈值标志

**原理**: 通过 clang driver `-mllvm` 前缀将参数传递给 LLVM 中端，影响 pass 的 cost model 决策。

```bash
clang -O3 -march=native -mllvm -slp-threshold=-4 gemm.c -o gemm
```

**发现方式**: 通过 `opt --help-hidden` 枚举所有 `-mllvm` 可接受的标志：
```python
discover_options_from_help(opt_path, pass_keywords)
# → {pass_name: [{flag, type, desc}]}
```

**核心参数列表**（LLVM 11，通过 `opt-11 --help-hidden` 验证）:

| Pass | 参数 | 默认值 | 效果 |
|------|------|--------|------|
| SLPVectorizerPass | `-slp-threshold` | 0 | 负值扩大 cost 接受窗口，允许更多 SLP 向量化 |
| SLPVectorizerPass | `-slp-min-tree-size` | 2 | 最小 SLP tree 大小 |
| LoopVectorizePass | `-vectorizer-min-trip-count` | 16 | 降低→允许短循环向量化 |
| LoopVectorizePass | `-small-loop-cost` | 20 | 小循环的 cost 上限 |
| LoopUnrollPass | `-unroll-threshold` | 150 | 增大→允许更激进展开 |
| GVNPass | `-gvn-max-hoisted` | 100 | 增大→允许更多冗余 load 提升 |
| LICMPass | `-licm-max-num-uses-traversed` | 8 | 增大→识别更多循环不变量 |
| InlinerPass | `-inline-threshold` | 225 | 增大→允许更激进内联 |

**禁止使用的标志**（`BLACKLISTED_FLAGS`）:
- `force-vector-width`, `force-vector-interleave`：绕过 cost model，强制向量宽度，不是"调参"而是"覆盖"
- `disable-loop-unrolling`, `disable-inlining`：关闭优化而非调优
- **论文合法性**: 只能调整 cost model 参数，不能绕过 cost model

### Channel 2: `#pragma clang loop` 元数据

**原理**: 在 for 循环前插入 pragma，编译器将其转为 `!llvm.loop` LLVM IR 元数据，影响该循环的 vectorizer/unroller 决策。

```c
// 向量化 hint
#pragma clang loop vectorize(enable) vectorize_width(8)
for (int j = 0; j < N; j++) { ... }

// 展开 hint  
#pragma clang loop unroll_count(4)
for (int i = 0; i < M; i++) { ... }
```

**实现**: `_apply_pragma_hints(source, hints)` 在 `optimize.py` 中通过行前缀匹配找到目标 for 循环，在其前一行插入 pragma。

**可用 pragma 列表**（`PRAGMA_LOOP_HINTS`）:
- `vectorize(enable/disable)`, `vectorize_width(N)`
- `interleave(enable/disable)`, `interleave_count(N)`  
- `unroll(enable/disable)`, `unroll_count(N)`
- `distribute(enable/disable)`

**使用约束**:
- 只对无循环携带依赖（loop-carried dependency）的循环使用
- **严禁**对 in-place stencil（如 Jacobi-2D）使用向量化 pragma → 语义错误
- **严禁**对三角循环（k < i）使用，因行程数不等无法向量化

### Channel 3: Clang Driver 高级标志

```bash
# 已在 O3 中默认启用的但可以额外指定
clang -O3 -march=native -fvectorize -fslp-vectorize
# 可以明确指定向量宽度（超过 O3 默认）
clang -O3 -march=native -mprefer-vector-width=512
```

**发现方式**: `discover_clang_driver_options()` 在 `tune_param.py` 中枚举 `CLANG_DRIVER_FLAG_GROUPS` 定义的高级标志并独立测试每个。

**为何不用 `-ffast-math`**: 改变 FP 结合律语义，导致数值验证失败（与 PolyBench reference 输出不一致）。

### Channel 4: 源码级 Kernel 重写

**原理**: LLM 直接重写 kernel 函数，通过结构变换暴露更多向量化机会，同时绕过无法通过参数解决的循环依赖。

**允许的变换**:
- **Loop Tiling/Cache Blocking**: 减小工作集大小，改善 cache 局部性
  ```c
  // Original: O(N³) in-order access
  for (i) for (j) for (k) C[i][j] += A[i][k] * B[k][j];
  // Tiled: T×T blocking
  for (i0,j0,k0 step T) for (i=i0..i0+T) for (j=j0..j0+T) for (k=k0..k0+T)
  ```
- **Register Blocking**: 标量 accumulator 替代多次内存访问
- **Loop Interchange**: 改善 innermost 循环的访存步长
- **Loop Fusion/Splitting**: 减少内存 pass 次数

**正确性验证**（双层）:
1. SMALL_DATASET 规模：数值精确对比（epsilon=1e-4）
2. STANDARD_DATASET 规模：数值对比（epsilon=2×1e-4）
超时自动降级为 MINI_DATASET。

**严禁**:
- 分裂归约 accumulator（改变 FP 结合顺序 → 数值不一致）
- in-place stencil 添加临时缓冲（改变算法语义）

### 改进建议

当前 `-mllvm` 参数通过 `opt --help-hidden` 发现，但缺少以下验证：
1. 参数确实能通过 `clang -mllvm` 传递（部分参数只对 `opt` 有效）
2. 参数值范围合理性检查（无法直接从 `--help-hidden` 得到）
3. 参数间的依赖关系（如 `slp-threshold` 影响 `slp-min-tree-size` 的实际效果）

**改进方向**: 对每个发现的参数做编译时 probe 验证（设置探针值 → 编译 → 检查 IR 是否变化），只向 LLM 展示确实有效的参数。这已通过 `src/ir_diff.py` 的 per-pass IR diff 部分实现。

---

## Section 3: Memory 管理分析

### 3.1 OptimizationHistory（核心状态对象）

```python
@dataclasses.dataclass
class StepRecord:
    step_num:   int
    action:     str    # try_flags | try_pragma | rewrite_source | done
    reasoning:  str    # LLM 推理（前 200 字符）
    speedup:    float
    strategy:   str
    error:      str
    flags:      list
    has_source: bool
    perf_stats: dict   # perf 硬件计数器快照
    snapshot_path: str
```

**关键特性**:
- `strategy_speedups: Dict[str, float]` — 每种策略类型的最优加速比，O(steps) 大小
- `failed_strategies: List[str]` — 黑名单（每步检查避免重复）
- `errored_strategies: List[str]` — 报错黑名单（编译错误等）
- **无界列表**: `steps: List[StepRecord]` 随步骤增长，无上限

### 3.2 Context Budget 管理

```python
TOKEN_BUDGET = 28000   # 留 4000 tokens 给 LLM 输出
```

**prompt 构建时各段估算**（基于 3mm kernel 典型值）:
| 段落 | 字符数（约） | token 数（约） |
|------|------------|--------------|
| ① 硬件环境 | 400 | 100 |
| ② perf + VTune | 600 | 150 |
| ③ 源码 | 2000 | 500 |
| ④ IR（80行） | 4000 | 1000 |
| ⑤ Pass pipeline | 800 | 200 |
| ⑤ Rich remarks（5个pass） | 3000 | 750 |
| ⑥ IR stats | 400 | 100 |
| ⑥ IR diff per-pass | 1200 | 300 |
| ⑦ History（8步详情） | 4000 | 1000 |
| **总计** | **~16400** | **~4100** |

在 28000 token 预算内有充足余量。

**截断优先级**（当超出预算时，越靠前越先截断）:
1. `static_summary`（有 perf 数据时冗余）
2. `ir_diff_table`（per-pass 修改表，补充信息）
3. `vtune_section`（重要但已有 perf 覆盖）
4. IR 文本（80 → 40 行，并添加截断标注）

### 3.3 已知问题与改进

**问题 1**: 截断时 LLM 未被通知
- **旧实现**: `ir_lines[:80] + ["... (truncated)"]` — 截断了但不明显
- **新实现**: `f"// [TRUNCATED: {trunc_count} lines omitted for context budget]"` — 明确告知

**问题 2**: History 无 token 预算限制
- `to_prompt_section()` 保留最近 8 步详情 + 早期摘要，但如果每步 reasoning 很长，仍可能超出
- **建议**: 对 `reasoning` 字段在记录时截断至 200 字符（已实现），对 history 总字符数增加上限

**问题 3**: perf_stats 在每个 StepRecord 中存储完整 dict
- 对于 20 步运行，perf_stats 累计约 3KB，可接受
- 长运行（>100步）时建议只存 IPC/LLC 两个关键指标

---

## Section 4: Prompt 有效性分析

### 4.1 各段内容索引

| 段落 | 内容 | 对 LLM 决策的价值 |
|------|------|-----------------|
| ① 硬件环境 | CPU 型号、SIMD 宽度、cache 大小 | 确定向量化目标宽度 |
| ② 性能计数器 | IPC、LLC miss、分支 miss + **VTune memory bound%** | 定位瓶颈类型 |
| ② 逆向推断 | perf 瓶颈 → O3 pass → 可调参数 | **核心**: 将抽象瓶颈映射到具体操作 |
| ③ 源码 | kernel 函数体（当前最优版本） | 代码结构理解 |
| ④ IR | O0 LLVM IR（80行，有截断标注） | 指令级结构，识别向量化阻碍 |
| ⑤ Pass pipeline | 执行顺序 + **fired/no-op 标注** | 知道哪些 pass 真正起了作用 |
| ⑤ **富 Remarks** | **新**: 精确到代码行/列/VF/IC/失败原因/源码片段 | 向量化失败的精确位置和原因 |
| ⑥ IR 统计 | vector_ops、phi 节点、fmul 等 | 当前优化程度量化 |
| ⑥ **IR diff per-pass** | **新**: 每个 pass 单独运行前后 IR 变化 | 确认 pass 是否有效 |
| ⑦ History | 每步 action/speedup/error，收敛检测 | 避免重复失败策略 |

### 4.2 有效机制

**逆向推断**（`get_targeted_passes()`）: 将 perf 硬件计数器直接映射到具体 pass 的具体参数，确保 LLM 有精准操作目标而非泛化建议。这是 beyond-O3 优化框架的核心创新点。

**History-based 避免**: `strategy_speedups`, `failed_strategies`, `errored_strategies` 构成三层黑名单，确保 LLM 不重复已失败路径。

**收敛检测**: 连续 4 步 speedup 差异 < 0.5% 时触发警告（`to_prompt_section()` line 238），提示 LLM 切换策略。

### 4.3 已识别的问题与改进

**问题 1: 收敛警告位置不显眼**
- 当前：附在 history 末尾，LLM 在长 history 中可能忽略
- **建议**: 移至 section ① guidance 开头，在步骤引导语中明确

**问题 2: try_flags 候选值缺乏依据**
- 当前：LLM 猜测候选值范围（如 `[-8,-4,-2,0]`）
- 改进：利用 `ir_diff.py` 的探针结果，只列出确实改变 IR 的参数值作为候选

**问题 3: 无 pass 间的因果关系**
- LLM 不知道 SLP 向量化依赖 SROA 完成后的 scalar 形式
- **建议**: 在 pass pipeline 段落加入前驱关系注释

**问题 4: 无 Prompt 版本追踪**
- 研究中需要能重现某次优化决策的完整 prompt
- **建议**: 将每步 prompt hash 存入 StepRecord

### 4.4 富 Remarks 格式（新实现）

原始文本 remarks 只显示行号和单行消息。新的 YAML-based 富 remarks 包含：

```
**SLPVectorizerPass** — 3 missed, 0 applied

  ❌ MISSED @ gemm.c:42:5 [in kernel_gemm]
     原因: loop not vectorized: cannot identify array bounds
     失败细节: cannot identify array bounds
     目标VF=8
     代码位置:
       40:   for (int i = 0; i < ni; i++) {
       41:     for (int j = 0; j < nj; j++) {
  >>>  42:       for (int k = 0; k < nk; k++) {
               ^
       43:         C[i][j] += A[i][k] * B[k][j];
```

字段来源：
- `line`, `column`: YAML `DebugLoc.Line`, `DebugLoc.Column`
- `function`: YAML `Function` 字段
- `vf`: YAML `Args[VectorizationFactor]`
- `ic`: YAML `Args[InterleaveCount]`
- `fail_reason`: YAML `Args[String]` 中包含 "not vectorized/cannot" 的部分
- `source_snippet`: 从源文件读取 ±2 行上下文，并标记 `>>>` 和 `^` 列指针

---

## Section 5: 回退机制设计

### 5.1 为何需要回退

source rewrite（`action=rewrite_source`）可能引入结构性退化，导致新版本比 O3 baseline 还慢。若后续步骤使用 `base="current_best"`，会在错误基础上继续累积变换，最终离最优解越来越远。

**场景示例**:
1. Step 3: rewrite → gemm_tiled.c（1.4× 加速，成为 best_source）
2. Step 4: rewrite → 在 gemm_tiled 上进一步 tile，bug → 0.6× 退化
3. Step 5: 若不回退，以 0.6× 版本为 base 继续，可能无法恢复

### 5.2 实现（已激活的 config 参数）

```python
rollback_stack: list = []   # List[Tuple[str|None, list, float]]
catastrophic_threshold = config.runtime.catastrophic_slowdown_threshold  # 默认 20.0%
```

**每步前**: `rollback_stack.append((best_source, list(best_flags), best_speedup))`

**每步后检测**:
```python
if (not result.get("error") and
        action not in ("done", "error") and
        best_speedup > 1.0 and rollback_stack and
        sp_raw < best_speedup * (1.0 - catastrophic_threshold / 100.0)):
    prev_src, prev_flags, prev_sp = rollback_stack[-1]
    best_source = prev_src   # 系统层面回退
    best_flags  = prev_flags
    # best_speedup 保持历史最优（未更新）
```

**触发条件**: 新 speedup < 当前最优 × (1 - 20%) = 当前最优的 80%

### 5.3 设计决策

**不回退 history**: LLM 必须看到失败记录（`failed_strategies`/`errored_strategies`）才能学习规避。回退 history 会让 LLM 重复同样的失败。

**只在 best_speedup > 1.0 时触发**: 如果从未找到改进，回退没有意义。

**config 参数映射**（均已在 `RuntimeConfig` 中定义）:

| 配置项 | 用途 | 默认值 |
|-------|------|--------|
| `catastrophic_slowdown_threshold` | 触发回退的退化百分比 | 20.0% |
| `backoff_slowdown_tolerance_pct` | 忽略小幅退化（噪声）| 3.0% |
| `seed_backoff_patience` | MCTS 中重复差结果的容忍次数 | 4 |
| `llm_backtrack_penalty_step` | 每步回退的 LLM penalty | 1.0 |
| `llm_backtrack_penalty_max` | 最大累积 LLM penalty | 4.0 |

注：`backoff_slowdown_tolerance_pct` 和 LLM penalty 参数目前在 agent 模式中未使用（MCTS 专用），agent 模式只用 `catastrophic_slowdown_threshold` 触发硬回退。

### 5.4 与 History 避免的区别

| 机制 | 层次 | 触发条件 | 效果 |
|------|------|---------|------|
| History-based avoidance | LLM 侧（软） | 任何失败/退化 | LLM 在下一步选择不同策略 |
| Rollback stack | 系统侧（强） | 灾难性退化（>20%） | 系统强制恢复到已知最优状态 |

---

## Section 6: 无数据集优化策略

### 6.1 触发条件

当 `find_polybench_utilities(src)` 返回 `None` 时触发静态分析兜底路径。常见原因：
- 自定义 benchmark 没有 `polybench.c` utilities
- 在新机器上只有源码没有完整 PolyBench 安装
- 对非 PolyBench 程序使用 COMET 框架

### 6.2 静态分析路径（`src/static_optimizer.py`）

**源码特征提取** (`analyze_source_patterns()`):

```python
patterns = {
    "loop_depth": 3,              # 最大循环嵌套深度（正则检测 for 结构）
    "innermost_stride": "row_major",  # 最内层变量在最后一维 → 好局部性
    "has_reduction": True,        # += / *= 在最内层
    "has_stencil": False,         # A[i+1][j] 式访问
    "has_triangular": False,      # k < i 式三角边界
    "array_count": 3,             # 访问的数组数量（A, B, C）
    "has_matmul_pattern": True,   # 深循环 + 归约 + 行主序
}
```

**IR 特征提取** (`analyze_ir_patterns()`，来自 O0 IR）:
```python
ir_patterns = {
    "phi_count": 6,           # phi 节点数（循环计数代理）
    "gep_count": 12,          # getelementptr 数（内存访问密度）
    "load_count": 8,
    "store_count": 2,
    "has_vector_types": False, # O0 IR 通常无向量类型
}
```

**瓶颈推断规则**（`infer_bottleneck_without_data()`）:

| 条件 | 推断瓶颈 | 置信度 | 建议 |
|------|---------|--------|------|
| loop_depth≥2, column_major | cache_inefficiency | 90% | loop interchange |
| stencil + loop_depth≥2 | memory_bound | 85% | cache blocking |
| matmul 模式, row_major, no stencil | vectorization_gap | 82% | SLP/loop-vec 调参 |
| 纯归约, no stencil | vectorization_gap | 70% | SLP threshold |
| 三角边界 | branch_overhead | 72% | loop peeling |
| GEP/phi 比 > 4 | memory_bound | 60% | 工作集分析 |

### 6.3 IR 提取可行性

IR 提取（`CompilerRunner.extract_ir()`）不依赖 PolyBench utilities，只需 clang 能编译源文件。即使没有 polybench.c，也可以：
1. 提取 O0 IR（`clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone`）
2. 提取 O3 pass pipeline（`opt -passes=default<O3> -debug-pass-manager`）
3. 提取 YAML remarks（`opt -passes=default<O3> -pass-remarks-output=...yaml`）

这意味着 pass 图可视化、IR diff 分析、pass 参数调整建议均可在无数据集情况下进行。

### 6.4 无数据集模式的局限

| 功能 | 有数据集 | 无数据集 |
|------|---------|---------|
| 量化加速比 | ✓ | ✗ |
| 数值正确性验证 | ✓（SMALL+STANDARD）| ✗（exit_only）|
| 硬件计数器 | ✓（perf + VTune）| ✗ |
| Pass 图可视化 | ✓ | ✓ |
| IR diff per-pass | ✓ | ✓ |
| YAML 富 remarks | ✓ | ✓ |
| 瓶颈推断 | 精确（测量）| 粗略（静态）|

### 6.5 论文注记

使用静态分析路径的优化结果不能与量化 profiling 结果直接对比。在论文评估表中应标注：
- 有 profiling 数据的结果标记为 "measured"
- 纯静态分析的结果标记为 "static"
- 两类结果应在不同表格或同表格不同列中展示

---

## 附录: 文件修改摘要

| 文件 | 修改类型 | 核心变更 |
|------|---------|---------|
| `src/vtune_analysis.py` | 新建 | VTune CLI 封装：采集→CSV解析→LLM摘要 |
| `src/ir_diff.py` | 新建 | per-pass IR 前后对比，FIRED/no-op 判断 |
| `src/static_optimizer.py` | 新建 | 无数据集静态源码/IR 分析，规则推断瓶颈 |
| `src/perf_analysis.py` | 修改 | 新增 `collect_combined_profile()`，VTune 优先于 perf 启发式 |
| `src/pass_graph.py` | 修改 | 节点新增 ir_changed/ir_skipped 字段，DOT 图新增绿框/灰填 |
| `tune_param.py` | 修改 | 新增 `extract_rich_remarks_yaml()`，YAML 富 remarks 含代码片段 |
| `optimize.py` | 修改 | 集成所有新模块；回退栈；静态分析兜底；prompt 新增 VTune/IR diff/富remarks 段 |
| `configs/config.yaml` | 修改 | vtune 配置注释完善 |
