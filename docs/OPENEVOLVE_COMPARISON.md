# COMET vs OpenEvolve 对照实验（PolyBench）

对照对象：[OpenEvolve](https://github.com/algorithmicsuperintelligence/openevolve)，
一个开源的进化式编码 agent（AlphaEvolve 复现）。本实验回答：**在相同的
LLM、相同的迭代预算、相同的测量与正确性纪律下，COMET 的"编译器反馈引导"搜索
是否优于 OpenEvolve 的"通用进化搜索"？**

> 状态：harness 已构建并单元测试通过（`tests/test_openevolve_compare.py`，5 例）。
> 实际计时运行**待消融扫描结束后**在 ARM 主机上执行——绝不能与消融并发，否则
> 共享内存带宽会污染两边计时。

## 1. 控制的变量（保证公平）

| 维度 | COMET | OpenEvolve | 是否匹配 |
|---|---|---|---|
| LLM | deepseek-v4-pro @ api.deepseek.com, temp 0.6, max_tokens 4000 | 同左（config.yaml 里写死同一模型/端点/参数） | ✅ |
| 迭代预算 | `--rounds 3` | `--iterations 3` | ✅（语义见 §3） |
| 编译器 | clang-21 -O3 | clang-21 -O3 | ✅ |
| 程序 | 30 PolyBench kernel | 同 30 个 | ✅ |
| 数据集 | LARGE_DATASET | LARGE_DATASET | ✅ |
| 目标代码区 | kernel_\<name\> 函数 | EVOLVE-BLOCK 精确包裹同一函数 | ✅ |
| 最终测量 | `confirm_result_external`（交替 n=5 + 外部墙钟 + pin-cpu）→ `decide_final_result` 回滚闸门 | **同一份代码**（driver 复用 `optimize.py` 的这两个函数） | ✅ |
| 正确性门 | DUMP_ARRAYS 逐值比对 | 同左（评估器内 + driver 金标准双重） | ✅ |
| CPU / 并发 | pin-cpu 2, 串行 | pin-cpu 2, `parallel_evaluations: 1`, 串行 | ✅ |

**唯一有意不同的变量：搜索策略本身。** COMET 用 pass/perf 反馈 + skill 引导的
agent 决策；OpenEvolve 用 diff-based 进化（种群/岛屿/精英选择）。

## 2. 关键设计点

- **防作弊**：OpenEvolve 的 fitness = 加速比，**但仅当数值正确**（评估器对错误
  输出返回 0）。driver 还做一次独立金标准复核，错误的进化胜者被强制记 1.0
  （`final_status=rejected_incorrect`）。这与 COMET 一致——没有系统被允许用"又快
  又错"的代码取胜。
- **测量口径统一**：两系统的**最终上报数字**都由 COMET 的
  `confirm_result_external` + `decide_final_result` 产生。in-loop fitness 各自不同
  （OpenEvolve 单/少次、COMET Phase-A 筛选），但那只影响搜索方向，不影响最终计数。
- **API key 不落盘**：config.yaml 里用 `${DEEPSEEK_API_KEY}`，OpenEvolve 运行时从
  环境变量解析，key 从不写进任何生成文件或日志。

## 3. 迭代语义的诚实说明

- COMET `--rounds 3` = 3 个 agent 决策步（每步可 try_flags / rewrite_source /
  try_pragma，且带失败反思与元规划）。
- OpenEvolve `--iterations 3` = 3 次进化迭代（每次 = 从种群采样一个程序 + 1 次 LLM
  diff + 1 次评估）。
- 两者都给 **3 次 LLM 引导的修改机会**，这是最接近的对齐。但机制不同：COMET 每步
  能看到编译器反馈并做结构化决策；OpenEvolve 靠种群多样性 + 进化选择。这个差异是
  实验**要测的东西**，不是需要消除的噪声。报告里会明说，不夸大"完全等价"。

## 4. 执行步骤（消融扫描结束后在 ARM 上跑）

```bash
# 0) 确认没有任何计时任务在跑
ssh ...132.145.22.86 'pgrep -af optimize.py'   # 必须为空

# 1) 克隆 + 装进独立 venv（不碰 COMET 的 .venv）
cd /home/hanning/comet
git clone --depth 1 https://github.com/algorithmicsuperintelligence/openevolve.git
python3 -m venv openevolve_compare/.venv
openevolve_compare/.venv/bin/pip install -e openevolve/

# 2) 生成 30 个程序的输入（路径指向 /home/hanning/comet/...）
.venv/bin/python scripts/openevolve_compare/gen_inputs.py --rounds 3 --pin-cpu 2

# 3) 跑对照（driver 用 COMET venv 以复用 confirm/gate；
#    openevolve 子进程用它自己的 venv）
.venv/bin/python scripts/openevolve_compare/run_compare.py \
  --openevolve-run openevolve/openevolve-run.py \
  --python openevolve_compare/.venv/bin/python \
  --rounds 3 --runs 5

# 4) 三方汇总 COMET-full / COMET-no_feedback / OpenEvolve
.venv/bin/python scripts/openevolve_compare/summarize_compare.py \
  --json-out openevolve_compare/summary.json
```

## 5. 文件

| 文件 | 作用 |
|---|---|
| `scripts/openevolve_compare/gen_inputs.py` | 每程序生成 initial_program.c(带 EVOLVE 标记)+evaluator.py+eval_config.json+config.yaml |
| `scripts/openevolve_compare/evaluator.py` | OpenEvolve 评估器：正确性门 + 加速比 fitness，测量口径同 COMET |
| `scripts/openevolve_compare/run_compare.py` | driver：跑 OpenEvolve → 复用 COMET 确认闸门重测最优 → 统一 schema 落盘 |
| `scripts/openevolve_compare/summarize_compare.py` | 三方对比表 + 配对 CI |
| `tests/test_openevolve_compare.py` | 标记插入 + config 匹配的单元测试 |

## 6. 待办 / 未决

- 实际计时运行未跑（等消融）。
- OpenEvolve 在 aarch64 上的安装未验证（依赖 openai/numpy/pyyaml，预期 OK，装完先
  跑 1 个程序 smoke 再全量）。
- 结果出来后并入 `docs/ABLATION_SNAPSHOT_*.md` 或独立结果文件。
