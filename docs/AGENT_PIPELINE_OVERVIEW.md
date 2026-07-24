# COMET Agent 全流程说明文档

**日期**: 2026-07-21
**目的**: 供日后快速浏览——不讲代码，只讲每个文件干了什么、整个 agent 怎么跑起来、编译器信息在哪个环节起作用。写作时点是本次大规模修复（gap-detection 联合选点、反思 LLM、SPEC 三个 benchmark 适配、最终确认测量 bug 修复）刚完成之后，所以这是目前为止最新、最准确的一版全景描述。`docs/COMET_INFO.md` 是 2026-07-03 的旧快照，里面提到的"待修复项"有一部分已经在这之后的会话里解决了，两份文档对照看时以本文档时间戳更新的部分为准。

---

## 1. 这个项目要做什么

COMET 是一个基于 LLM 的编译优化 agent。目标是在 `clang -O3 -march=native` 已经优化过的基础上，再进一步压榨性能（"beyond-O3"）。给定一个 C 语言的计算核心（kernel），agent 会自动、多轮迭代地尝试三类手段——调编译器参数、加循环 pragma 提示、直接重写源码——每一轮都基于真实的编译器反馈（性能计数器、优化日志、IR 变化）做决策，而不是盲目搜索。

最初只针对 PolyBench/C 套件（30 个规整的线性代数/stencil kernel），现在已经扩展到能处理任意 C 程序：TSVC（向量化测试套件）、cBench（真实小程序集合）、SPEC CPU2017 的部分 C 语言 benchmark（mcf_r、lbm_r、nab_r，本次会话新增）。

---

## 2. 顶层目录结构 & 每个文件的角色

```
comet/
├── optimize.py          agent 主循环入口，一切都从这里发起（4700+ 行，核心文件）
├── tune_param.py         "调参"通道的底层工具箱（被 optimize.py 大量导入复用）
├── tune_source.py        "源码重写"通道的底层工具箱（同样被 optimize.py 复用）
├── src/                  共享基础设施——编译器交互、性能分析、正确性验证、日志
│   ├── config.py                读取 configs/config.yaml，产出各模块用的 dataclass 配置
│   ├── compiler_manager.py      CompilerRunner：统一封装 clang/opt 调用路径
│   ├── llm_client.py            LLM API 调用封装（OpenAI 兼容接口）+ JSON 响应清洗
│   ├── build_utils.py           编译 + 计时的共享底层函数（run_timing、compile_c）
│   ├── correctness.py           通用正确性验证（numeric/hash/exit_only 三档，不依赖 PolyBench 宏）
│   ├── remarks.py                LLVM 优化 remarks 抽取（YAML 格式，逐行定位）
│   ├── perf_analysis.py         perf stat 硬件计数器采集 + 瓶颈分类 + 反向映射到 pass 参数
│   ├── vtune_analysis.py        Intel VTune 命令行封装（perf 的补充，可选）
│   ├── ir_diff.py                判断每个 pass 是否真的改动了 IR（区分"跑过"和"起作用"）
│   ├── pass_graph.py            提取 -O3 pass 执行序列，画出 pass pipeline 图
│   ├── pass_utils.py            构造不同版本 LLVM opt 命令行的小工具
│   ├── hotspot.py                热点函数探测：kernel 入口本身不算热点时，找真正的热点函数
│   ├── static_optimizer.py      没有运行时数据时的兜底：纯静态源码/IR 模式识别
│   ├── static_analyzer.py       RemarksExtractor：更早期的 remarks 抽取类（辅助）
│   ├── diagnostics.py            清理 clang 报错信息给 LLM 看（去掉人类专用的 caret 下划线）
│   ├── datasets.py               判断一个 kernel 属于 polybench/tsvc/cbench 哪个数据集
│   ├── data_structures.py       Remark/IRStats 等共享数据类定义
│   ├── polybench_paths.py       统一查找 PolyBench/TSVC_shim/CBench_shim 的 utilities/ 目录
│   └── run_logger.py            每次运行的完整日志落盘（stdout、LLM 调用记录、结果 JSON）
└── scripts/
    ├── gen_spec_kernels.py       把 SPEC CPU2017 的 C benchmark 包装成本系统能跑的 kernel
    ├── gen_tsvc_kernels.py       把 TSVC 的每个循环包装成一个 kernel
    └── gen_cbench_kernels.py     把 cBench 的每个程序包装成一个 kernel
```

**一个关键设计**：`optimize.py` 本身不实现"怎么编译""怎么测正确性""怎么抽 remarks"这些底层操作，而是从 `tune_param.py`、`tune_source.py`、`src/*.py` 里 import 复用。这是历史遗留——`tune_param.py`/`tune_source.py` 曾经是各自独立能跑的命令行工具（"只调参"和"只重写源码"两条腿），现在它们的 `main()` 入口基本废弃不用，但里面的函数被当作工具箱持续使用。

---

## 3. Agent 主循环：一次运行从头到尾发生了什么

以 `python3 optimize.py --program some_kernel.c --rounds 5 --runs 3` 为例：

### 3.1 准备阶段（跑一次，不循环）

1. **数据集识别**（`src/datasets.py`）：根据源文件路径判断这是 PolyBench 原生 kernel，还是 TSVC/cBench/SPEC 生成的"shim"（伪装成 PolyBench 形状的包装）。
2. **证据收集**（`optimize.py: collect_all_evidence`）——这是整个系统"看懂编译器在干什么"的核心，一次性做完，后续每一步都复用：
   - 用 `clang -O0` 生成未优化 IR，再跑 `opt -O3 -debug-pass-manager` 抓取完整 pass 执行序列（`src/pass_graph.py`）。
   - 对 pipeline 里的每个 pass，单独在 O0 IR 上跑一遍并对比 IR 统计量前后差异，判断这个 pass **是真的起了作用还是空跑**（`src/ir_diff.py`）——这比"这个 pass 跑没跑"（`-debug-pass-manager` 能看到的）信息量大得多。
   - 用 `-Rpass=.* -Rpass-missed=.* -Rpass-analysis=.*` 跑一次编译，抽取逐行、带源码片段、带向量化因子(VF)/交织因子(IC)、带具体失败原因字符串的富 remarks（`src/remarks.py`）。
   - 编译出 baseline 二进制，跑 `perf stat` 拿到 IPC、LLC miss 率、分支 miss 率等硬件计数器，分类出瓶颈类型（内存瓶颈/向量化不足/循环开销大/内联不足/冗余计算），并**反向映射**到"这个瓶颈类型该重点看哪些 pass 的哪些参数"（`src/perf_analysis.py`，第 6 节详细展开）。
   - 用 `opt --help-hidden` **动态发现**这次编译实际用到的 pass 有哪些可调的 cost-model 参数（不是写死的参数列表——LLVM 版本升级、pass 改名都不需要改代码）。
   - **热点函数探测**（`src/hotspot.py`，第 5 节详细展开）：kernel 入口函数本身可能只是个壳（比如 SPEC mcf_r 的 `kernel_mcf_r` 做完 I/O 就调用 `global_opt()`，真正的计算在更深的调用链里），这一步找出真正该被重写的函数，整个 run 只算一次、缓存起来，避免"决策提示词看到的是入口函数证据，但实际改的是热点函数代码"这种意图和实现对不上的错位。
3. 把上面所有信息拼进一份结构化 prompt 交给 LLM，第一步强制走 `try_flags`，第二步强制走 `rewrite_source`（保证两个通道各自都被独立测过一次），第三步起由 LLM/元规划器自由选择。

### 3.2 每一步的循环体（重复 `max_steps` 次，或 LLM 主动喊 "done" 提前结束）

```
for step in 1..max_steps:
    确定这一步是否有"强制动作"：
        step==1 → try_flags
        step==2 → rewrite_source
        同一工具连续用了2次 → 强制换别的（反重复兜底）
        否则 → 从"计划序列"缓冲区取下一个（每3步问一次元规划 LLM，见第7节）
    保存当前 (best_source, best_flags, best_speedup) 快照，用于灾难性退化回退
    result = run_agent_step(...)      # 一次决策 LLM 调用 + 实际执行该 action
    history.record(step, result)      # 记入历史，供后续步骤参考
    若本步没改进/出错 → 触发反思 LLM（reflect_on_failure，本次新增，见第8节）
    若本步比当前最优差 20% 以上 → 系统状态回退到上一快照（history 不回退，LLM仍能看到这次失败用于规避）
    若 action == "done" → 结束循环
    若本步 speedup 优于 best_speedup → 更新 best_source/best_flags/best_speedup
```

每一步的决策 LLM 调用看到的 prompt 包含（`_build_agent_prompt`）：硬件环境、性能计数器+反向推断结论、当前最优源码、LLVM IR 片段、pass pipeline 图（区分起作用/空跑）、富 remarks、完整历史记录（含反思结论）、当前热点目标提示。LLM 返回一个 JSON，指定 action（`try_flags`/`try_pragma`/`rewrite_source`/`done`）+ 具体策略描述 + 自我预测的 `improvement_analysis`。

### 3.3 收尾阶段

1. 如果 source 重写和 flags 调参从未一起测过（"compound"），补测一次两者叠加的效果。
2. **最终确认测量**：交替测量 baseline 和最优候选各 N 次（`base, best, base, best, ...`），取配对比值的中位数，抵消运行过程中的系统性漂移（温控降频、其他进程负载）。本次会话刚修复了这里的一个严重 bug（见第 9 节）。
3. 输出最终报告：baseline 耗时、最优加速比、confirmed_speedup（带 IQR 和变异系数）、用到的 flags、结果 JSON 路径、快照目录路径。

---

## 4. 三个优化通道详解

- **Channel 1 — try_flags（调参）**：只允许调整 `-mllvm` 传进去的 pass **cost-model 阈值**参数（例如 `--slp-threshold=-4`、`--licm-mssa-optimization-cap=100`），不允许绕过 cost model 本身的黑名单参数（如强制向量宽度）。分两阶段搜索：Phase A 对每个候选参数独立测（单次快速筛选），Phase B 把 Phase A 里表现最好的 2-3 个参数组合起来联合测（因为参数之间可能有交互效应，各自最优不等于组合最优）。搜索候选值来自"动态发现"的真实可调参数，不是写死列表。
- **Channel 2 — try_pragma（循环提示）**：在目标循环前插入 `#pragma clang loop vectorize(enable) vectorize_width(N)` / `unroll_count(N)` 之类的提示，转成 `!llvm.loop` 元数据，让编译器在保留正确性检查的前提下更激进地做某个变换。
- **Channel 3 — rewrite_source（源码重写）**：LLM 直接重写 kernel 函数体本身——循环分块、寄存器分块、循环交换、循环融合等。这是唯一允许改变代码结构的通道，也是收益上限最高、风险也最高的通道，因此有两级数值等价性检查兜底（见第 10 节）。

**多函数联合重写**（本次会话新增能力）：当热点探测发现性能瓶颈分散在多个分数相近的函数里（而不是集中在一个函数），rewrite_source 会一次性对这一组函数生成统一的改写方案（`_build_rewrite_impl_prompt_multi`），而不是只能孤立地改一个函数、看不到函数间的调用关系。哪些函数该被联合改写，由第 5 节的 gap-detection 机制自适应判定。

---

## 5. 热点探测 + 自适应联合选点（`src/hotspot.py`）

**为什么需要这个模块**：像 SPEC mcf_r 这种真实程序，kernel 入口函数往往只是个壳（读输入、调用真正的求解器、写输出），把它当成"要优化的函数"去重写，改的是几乎不影响性能的代码。这个模块沿着调用图走（最多 4 跳，最多考察 60 个函数），给每个候选函数打分：是否在循环内被调用、自身是否包含循环、算术密度（含指针解引用和比较运算，按权重计入）、是否有 I/O 调用（倒扣分）。

**自适应联合选点**（本次会话重写，是这次修复里最重要的一处泛化性改进）：早先的版本用写死的"分数在最高分 15% 以内、最多选 3 个"作为联合改写的判定标准，这个阈值是照着 mcf_r 一个案例手调出来的，用户明确指出这不该是死板的固定数字。现在改成了**基于分数分布本身的 gap-detection**：把候选按分数从高到低排序，找相邻分数之间**相对下降幅度最大**的那个断点，断点之前的所有候选归为一类一起改写；如果最大的断点幅度本身都很小（默认阈值 8%），说明分数分布太平、没有真正的结构性分界，退化为只选最高分那一个（不联合）。这个算法用真实数据验证过三个完全不同的 benchmark，全部符合预期且没有为任何一个案例单独调参：mcf_r 选出 primal_iminus/switch_arcs/markBaskets 三个函数、lbm_r 选出 LBM_performStreamCollideTRT/LBM_handleInOutFlow 两个、nab_r 选出 mme_init/md 两个。

---

## 6. 编译器信息具体在哪些环节起作用

这是这个系统区别于"纯 LLM 瞎猜"或"纯随机搜索"的核心——每一轮决策都建立在真实、结构化的编译器反馈上，而不是让 LLM 凭直觉判断：

1. **Pass pipeline 图**（`src/pass_graph.py`）：告诉 LLM "-O3 这次到底跑了哪些 pass、按什么顺序"，而不是让它假设一个通用的优化流程。
2. **IR diff**（`src/ir_diff.py`）：区分"这个 pass 执行过"和"这个 pass 真的改了 IR"——很多 pass 会因为 cost model 判断不值得而空跑，只看 `-debug-pass-manager` 的执行记录会误导 LLM 以为某个优化已经发生了。
3. **富 remarks**（`src/remarks.py`）：clang 默认的 remark 只有一行提示，这里抽取的版本精确到代码行/列、带 ±2 行源码片段、带向量化因子、带具体的失败原因字符串（比如"cannot identify array bounds"），让 LLM 知道**具体是哪一行、因为什么原因**没被优化，而不是笼统地"这里没向量化"。
4. **perf stat 硬件计数器 + 反向瓶颈映射**（`src/perf_analysis.py`）：采集到 IPC 低、LLC miss 率高之后，不是简单报告给 LLM 让它自己联想，而是程序化地把"内存瓶颈"这个分类结果映射到具体该看哪几个 pass 的哪些参数（比如内存瓶颈优先看 `LICMPass`/`LoopInterchangePass`/`LoopDistributePass`），把"发现了什么问题"和"该往哪个方向调"直接连起来。
5. **动态参数发现**（`tune_param.py: discover_options_from_help`）：调用 `opt --help-hidden` 现场查询当前 LLVM 版本实际支持哪些 cost-model 参数，而不是写死一份参数列表——LLVM 版本升级、pass 重命名都不需要改代码去适配。
6. **VTune（可选补充）**（`src/vtune_analysis.py`）：perf stat 之外，如果机器上装了 Intel VTune，会额外采集内存受限百分比、真实的硬件级热点函数排名。

---

## 7. 元规划 LLM（Planning）

`optimize.py: plan_action_sequence`。每隔几步（缓冲区耗尽时）单独问一次 LLM："接下来最多 3 步该按什么顺序用 try_flags/try_pragma/rewrite_source"，目的是避免同一个工具被连续使用、保证三个通道都被覆盖到、并且能根据"最近几步 speedup 是否停滞"这类信号调整策略（比如连续两步都在调参没进展，就该换成源码重写而不是继续调参）。

本次会话之前，这个模块几乎每次调用都失败：底层是个推理模型，会把大段思维链输出到单独的 `reasoning_content` 字段，只有想清楚了才写最终的 `content`；而这里给的 token 预算（512）小到模型思考都没写完预算就用光了，`content` 一直是空的，兜底逻辑把未写完的思维链原文当结果返回，导致 JSON 解析必然失败。修复方式：把预算提到 2000，并且加了一层"就算返回的还是夹杂了叙述文字的响应，也去扫描找第一个配对完整的 `{...}` JSON 块解析"的兜底。修复后已经在真实运行日志里验证过：能正常输出规划序列并被主循环执行。

---

## 8. 反思 LLM（Reflection，本次会话新增）

在这次会话之前，系统里**没有独立的反思机制**——唯一接近"反思"的东西是决策 LLM 自己在选定 action 的**同一次调用里**顺带预测"如果这步没改进，可能是因为什么"（`improvement_analysis` 字段），这本质上是决策前的猜测，不是基于真实结果的事后诊断。

新增的 `reflect_on_failure`（`optimize.py`）是一次**独立的 LLM 调用**，只在某一步真的执行完、且结果是"失败或没有超过当前最优"时触发，喂给它的是**真实发生的数据**：具体的编译/解析错误信息，或者真实测得的 speedup 和 perf 计数器变化。输出的诊断文本存进这一步的历史记录，通过历史记录自动出现在下一步的决策 prompt 里，让下一步的决策 LLM 和元规划 LLM 都能看到"上一次为什么没用、下次该换什么思路"。

已经在 nab_r 的完整真实运行里验证：两次 rewrite_source/try_pragma 失败后都准确触发，诊断出的根因（工具链解析函数名与源码跨度不匹配、pragma 没找到匹配的循环起始位置）和给出的替代方案（改用手动加 pragma、换更简单的变换）都合理可执行，没有出现过泛泛而谈或答非所问的情况。

---

## 9. 测量方法论（这是这次会话花了大量精力修的地方）

系统里**同时存在两套完全独立的计时方法**，混用是历史上多次出现"数字看起来离谱"（比如报出 134x 这种不可能的加速比）的根源：

- **外部墙钟计时**（`src/build_utils.py: run_timing`，以及 `optimize.py` 里的 `_single_shot_ms_external`/`confirm_result_external`）：Python 侧用 `time.monotonic()` 包住整个子进程调用过程，不关心程序自己打印了什么。这是**唯一对任意程序都可靠**的方法，因为它根本不依赖程序输出的格式。
- **程序自报时间**（已删除的 `_single_shot_ms`/`confirm_result`）：解析程序 stdout 最后一个空白分隔的 token，当作耗时（秒）——这个约定只对 PolyBench 原生 kernel 成立，因为 PolyBench 的 `polybench_print_instruments` 就是按这个格式打印的。

本次发现并修复的 bug：**最终确认测量**（"确认加速比"，本该是最终汇报用的可信数字）之前一直用的是程序自报时间这套方法。对 SPEC benchmark 这种会打印自己业务输出的程序，最后一个 token 根本不是耗时值——nab_r 的最后一个 token 是"...Done, md returns 0"里的那个 `0`，导致最终确认**每次都直接失败**（回退到没有二次确认的单次测量结果）；更隐蔽的是 lbm_r 曾经"成功"报出过一个 1.0000x、方差 0.0% 的确认结果——不是真的测出了 1.0x，而是恰好 base 和 best 是同一份代码，两边解析出的假数字碰巧相等，给出了一个看起来正常、实际毫无意义的"确认"。已经把最终确认的调用点切换成外部墙钟方法（和 try_flags 阶段的复测本来就在用的方法统一），手动验证过：同一个二进制自己比自己，能正确给出接近 1.0x、带合理小噪声的结果，而不是直接失败。旧的自报时间实现已经整体删除（不再有任何调用点）。

---

## 10. 正确性验证（`src/correctness.py`，替代了早期 PolyBench 专用的验证方式）

三档验证，自动探测选用哪一档：

| 档位 | 检查方式 | 什么情况下选用 |
|---|---|---|
| `numeric` | 抽取输出里所有数字 token，逐元素做相对误差比对 | 参考输出里能解析出数字 |
| `hash` | 输出字节的 SHA256 精确匹配 | 输出确定性但不是有意义的数字（比如文本/二进制数据） |
| `exit_only` | 只看进程退出码是否为 0 | 前两者都不适用（不确定性输出或空输出） |

这套机制不要求目标程序 `#include <polybench.h>` 或调用任何特定宏——直接对程序原生的 stdout/stderr（加可选的输出文件）做检查，这是能让 TSVC/cBench/SPEC 这些"本来跟 PolyBench 毫无关系"的程序也能被这套系统优化的关键前提（详见 `docs/GENERIC_HARNESS_DESIGN.md`）。

`tune_source.py: compare_outputs()` 是更早期、专门给 PolyBench 原生 kernel 用的数值比对函数（`epsilon=1e-4`，要求绝对误差和相对误差**同时**超标才判定不一致），rewrite_source 通道内部的两级检查（SMALL_DATASET 快速筛、STANDARD_DATASET 复核）仍在用这个。

---

## 11. LLM 交互层（`src/llm_client.py`）

`LLMClient` 封装了一个 OpenAI 兼容接口的调用，处理超时重试、以及一个关键细节：这个系统用的是**推理模型**，会把思维链输出到独立的 `reasoning_content` 字段，只有想清楚了才写 `content`。如果调用方给的 `max_tokens` 太小，模型可能思考没写完预算就耗尽，导致 `content` 是空的——这时兜底逻辑会把 `reasoning_content` 的原始文字当结果返回（好过完全没有响应），但下游如果要求严格 JSON 格式解析就会失败。这个坑在本次会话里在两个不同的调用点（元规划 LLM、最初的多函数重写实现）都实际踩到过，教训是：**任何要求 LLM 返回结构化 JSON 的调用点，都要给足够的 token 预算，并且做"扫描找第一个完整 JSON 块"这种兜底解析**，不能假设模型永远会把 JSON 干净利落地放在响应最前面。

---

## 12. 日志与运行记录（`src/run_logger.py`）

每次运行会在 `runs/{时间戳}_{程序名}/` 下留下四份文件：`full.log`（完整 stdout）、`llm_calls.jsonl`（每次 LLM 调用的完整 prompt + 响应，逐行 JSON）、`results.json`（最终结果结构化数据）、`compile_cmds.log`（每条编译命令和结果）。这是复现某次运行、事后排查某一步为什么失败的主要入口。

---

## 13. 三个 shim 生成脚本（怎么把"不是 PolyBench 的程序"接进这套系统）

系统的核心假设是每个 kernel 都长成"一个 `kernel_<name>()` 函数 + 私有的 `utilities/polybench.c`"这个形状。三个生成脚本各自负责把不同来源的真实程序改造成这个形状：

- **`gen_tsvc_kernels.py`**：TSVC 每个循环（s000、s111...）本来就是独立的小函数，改造量最小——重命名、接一个打印校验和的 `main`。
- **`gen_cbench_kernels.py`**：cBench 的每个程序按两种约定处理——用 CK 框架的（`ctuning-rtl.c` + `main1(...)`）丢掉框架文件、重命名 `main1`；用普通 `main(argc, argv)` 的直接重命名。其余源文件拼进程序私有的 `polybench.c` 副本。
- **`gen_spec_kernels.py`**（本次会话重点工作对象，新增 nab_r）：SPEC CPU2017 的 benchmark 结构差异很大，每个都是手写配置（`entry_file`、`sources` 列表、`argv`、可选的 `rundir`/`extra_defines`/`text_fixups`）。核心做法是把除入口文件外的所有源文件做**unity build**（拼接进一个翻译单元），这样能直接复用现有的单文件编译假设，但代价是原本分开编译时互不冲突的东西（头文件里没写 include guard 的 typedef、不同文件里恰好同名的 static 辅助函数、返回类型写错但从没被 linker 检查过的过期声明）全都会在同一个 TU 里炸出来。这次会话为 nab_r 修的几个 bug（`engine.ih` 被误加 include guard、`reducerror`/`select_atoms`/`dist`/`get` 的符号冲突）都是这一类问题，新增的 `text_fixups` 配置项就是给这类"某个文件需要打一个针对性文本补丁"场景用的通用机制。

目前已适配：mcf_r、lbm_r、nab_r 三个 SPEC 基准。已调研但未实现的：xz_r（两个 `main`，约70个文件）、imagick_r/perlbench_r/gcc_r（SPEC 自带的 harness 分发模式，文件数量大）、所有 Fortran/C++ 语言的 benchmark（这套系统只驱动 clang -mllvm 参数，天然不适用）。

---

## 14. 已知局限（截至本文档时间戳仍然成立的部分）

- **搜索过程本身没有方差数据**：LLM 调用有温度（非零），agent 自己的动作序列是随机的，目前每个 benchmark 只完整跑一次 agent 搜索，没有独立重复验证过"这个结果是稳定的还是运气好的一次 rollout"。
- **没有非 LLM 对照组**：目前唯一的对照是 `-O3` baseline，没有跟同等预算下的随机搜索/贝叶斯优化比较过，无法单独论证"是 LLM 的决策质量带来的收益"而不是"随便搜 15 步都能找到"。
- **没有消融实验**：富 remarks、回退机制、元规划、反思 LLM 这几个组件各自的贡献目前都没有被单独隔离测量过。
- **计时用的数据规模（LARGE_DATASET）从未做过正确性检查**：正确性验证在 SMALL/STANDARD 规模上做，真正报告的加速比数字是在 LARGE_DATASET 上测的，两者之间存在"小规模正确、大规模因为越界之类的规模相关 bug 而出错"的理论风险敞口，目前没有堵上。
- **工具链固定在 LLVM/Clang 11**（2020年版本）：这里发现的优化空间在更新版本的 `-O3` 上是否依然存在，没有验证过。
