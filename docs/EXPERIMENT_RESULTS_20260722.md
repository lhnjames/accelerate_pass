# 2026-07-22 快速实验记录

本记录区分三类结果：基线 smoke test、真实调优结果、以及搜索对照。没有候选参数被测量时，不把 `1.000x` 解释成“优化无效”。

## 远程 LLVM 21 CBench/PolyBench 基线

远程主机为 `ubuntu@132.145.22.86`，CPU 为 4 核 aarch64 Neoverse-N1；使用 `/usr/bin/clang-21`，固定 CPU0，独立重复 5 次。以下是直接二进制的外部墙钟测量，均返回退出码 0。

| benchmark | 数据集 | n | mean (ms) | stdev (ms) | CV |
|---|---|---:|---:|---:|---:|
| bzip2_encode | CBench | 5 | 97.562 | 0.712 | 0.73% |
| automotive_susan_corners | CBench | 5 | 6.549 | 0.718 | 10.97% |
| automotive_susan_edges | CBench | 5 | 10.480 | 0.413 | 3.94% |
| 3mm (`LARGE_DATASET`) | PolyBench | 5 | 8787.837 | 72.798 | 0.83% |
| nussinov (`LARGE_DATASET`) | PolyBench | 5 | 10970.831 | 134.051 | 1.22% |

COMET 基线证据同时记录了 63 个 O3 pipeline pass；bzip2、susan、3mm、nussinov 的正确性模式为 numeric（susan corners 为 hash）。远程 `perf` 输出已改为容错 UTF-8 解码，bzip2 复测能产生 IPC/LLC/瓶颈证据。

原始日志目录：`/home/hanning/comet/quick_remote_20260722/repeat/`；二进制重复样本位于 `repeat/*.samples`。

## SPEC2017 smoke timing

当前服务器已有的多 TU manifests 在本机上完成了 test workload 的 3 对重复；正确性全部通过。candidate_flags 为空，因此这是 toolchain/manifest/variance smoke，不是 LLM 优化结果。

| benchmark | correctness | paired speedups (3) | median |
|---|---|---|---:|
| namd_r | pass (numeric) | 1.0372, 1.0586, 1.0000 | 1.0372x |
| deepsjeng_r | pass (numeric) | 1.0001, 1.0714, 1.0308 | 1.0308x |
| leela_r | pass (hash) | 1.0103, 0.9898, 0.9627 | 0.9898x |

结果 JSON：`SPEC_quick_runs/20260722_*_r3/experiment.json`。

远程 SPEC shim 中尚未排除的可运行样本 `specrand_ir` 已通过 LLVM21 基线分析，10.94 ms、numeric correctness；mcf_r/lbm_r/nab_r 保留为已完成记录，Fortran benchmark 不纳入此链路。

另外在远程 SPEC 原始源码上直接用 `clang++-21 -O3 -std=gnu++03` 完成了三个 C++ test smoke（固定 CPU0、3 次重复）：`deepsjeng_r` 6791.134±11.179 ms、`leela_r` 4300.336±11.671 ms、`namd_r` 4080.935±8.542 ms。deepsjeng/leela 输出与 SPEC test 参考输出完全匹配；NAMD 仅有 `-0.000000` 与 `0.000000` 的 signed-zero 文本差异，规范化后匹配。二进制和日志位于远程 `quick_remote_20260722/spec_cpp/`。

补充的 SPEC C 基准 `525.x264_r` 也已用 clang-21 `-O3` 编译。test workload 的解码和 156 帧编码均返回 0；官方 `imagevalidate_525` 对 frame 50、100、155 全部通过（AVG SSIM=1.000000000）。固定 CPU0 的编码重复时间为 16.78、16.66、16.94 s，均值 16.793 s、样本标准差 0.140 s、CV 0.84%。该结果仍是 LLVM21 O3 基线，不是 LLM pass 优化结果；原始产物与日志位于远程 `quick_remote_20260722/spec_c/x264_test/`。

再补充 `557.xz_r`：clang-21 `-O3` test workload（4 MiB buffer、level 0）3 次固定 CPU0 运行均返回 0，时间为 0.63、0.62、0.61 s，均值 0.620 s、样本标准差 0.010 s、CV 1.61%。程序报告输入 SHA-512、压缩大小范围和解压后数据均正确；原始日志位于远程 `quick_remote_20260722/spec_c/xz_test/`。

此外，`520.omnetpp_r` 已通过远端 SPEC 官方 harness 的多 TU build/run/compare 流程。临时配置明确使用 `/usr/bin/clang-21` 与 `/usr/bin/clang++-21`、`-O3`；test workload 三次均显示 `S`（success/correctness pass），Run Time 为 4.58、4.55、4.64 s，均值 4.590 s、样本标准差 0.046 s、CV 1.00%。官方报告为远端 `spec2017/result/CPU2017.006.intrate.test.txt`，不是 LLM 优化结果。

`511.povray_r` 也通过同一官方多 TU 流程（C++/C 混合，clang++-21/clang-21）。test workload 三次均为 `S`，Run Time 为 0.320、0.320、0.309 s，均值 0.316 s、样本标准差 0.006 s、CV 2.01%；报告位于远端 `spec2017/result/CPU2017.008.fprate.test.txt`。

`526.blender_r` 初次 C++03 构建因 GCC11 `numeric_traits.h` 与旧标准不兼容而失败；切换该 benchmark 专用配置到 `clang++-21 -std=gnu++11 -O3` 后多 TU 构建成功。官方 test 运行三次均为 `S`，时间 0.275、0.268、0.228 s，均值 0.257 s、样本标准差 0.025 s、CV 9.87%；`imagevalidate_526` compare 返回 0。报告位于远端 `spec2017/result/CPU2017.011.fprate.test.txt`。该高 CV 主要来自 test workload 很短，应在 train/ref 上重复后再作性能结论。

## 搜索对照状态

`scripts/run_search_baseline.py` 已在 3mm 小数据 smoke 配置上完成同预算对照：共享 LLVM21 catalog 625 个候选轴组合、每个 seed 2 个候选、`runs=1`，并用 `-DPOLYBENCH_DUMP_ARRAYS` 做 correctness gate。五个 seed（101/211/307/401/503）均无失败候选。

| method | seed | budget | failed | best objective |
|---|---:|---:|---:|---:|
| seeded random | 101/211/307/401/503 | 2 each | 0/10 | median 0.9996x, IQR 0.0033 |
| Bayesian GP + EI | 101/211/307/401/503 | 2 each | 0/10 | median 1.0005x, IQR 0.0143 |

原始结果：`results/baseline_search_3mm_smoke_5seed_20260722/search_baselines.json`，汇总：`summary.json`。五个 seed 的成功率（speedup > 1.01）均为 0；这是小数据等预算 smoke，对正式大数据的性能结论仍需单独重复。

此前第一次 3mm 大数据配置把 `POLYBENCH_TIME` 带入 correctness binary，导致所有候选被正确性门拒绝（numeric mismatch，最高相对误差约 6.86e-2）；该失败暴露出 timing defines 与 correctness defines 必须分离，现已用 correctness-only dump 配置重跑小数据并记录。

为避免同类误用，`run_search_baseline.py` 现在会在 correctness 编译中自动剔除 `-DPOLYBENCH_TIME`（timing 编译仍保留原始 `--define`），并把两套 define 写入结果元数据；对应单元测试已覆盖。

## 本地 PolyBench 全自动流程审计

用修复后的 `run_polybench.sh` 运行了全部 30 个 PolyBench 程序：

```text
ROUNDS=1 RUNS=1 PIN_CPU=39 CONCURRENCY=3 QUICK_CHECK=1 \
  LOGDIR=logs_polybench_full_20260722 bash run_polybench.sh
```

30 个任务均被调度并完成；28 个正常返回，`lu`/`ludcmp` 首次因旧的 60 s timing hard limit 被误判。将 `run_timing` 的 warmup/测量超时改为可配置、默认 600 s 后，两个 solver 以 `ROUNDS=0` 重跑均成功：`lu` 基线 34532.30 ms，`ludcmp` 基线 34893.87 ms。所有成功任务的 correctness mode 均为 numeric。

下面的“加速比”必须谨慎解释：本机 DeepSeek API 在该批次全部返回 connection error，28 个 `ROUNDS=1` 任务没有候选参数/源码改写，因此 `1.0000x` 是“未执行优化候选”的占位基线，不是优化效果。（该 API 故障在当日下午已恢复，真实优化结果见本文件“真实 LLM 优化结果”一节。）

| dataset | baseline (ms) | reported speedup | status |
|---|---:|---:|---|
| correlation | 11000.59 | 1.0000x | baseline-only |
| covariance | 10962.80 | 1.0000x | baseline-only |
| gemm | 482.35 | 1.0000x | baseline-only |
| gemver | 48.89 | 1.0000x | baseline-only |
| gesummv | 50.16 | 1.0000x | baseline-only |
| symm | 2464.52 | 1.0000x | baseline-only |
| syr2k | 4154.51 | 1.0000x | baseline-only |
| syrk | 817.52 | 1.0000x | baseline-only |
| trmm | 2298.65 | 1.0000x | baseline-only |
| 2mm | 3697.00 | 1.0000x | baseline-only |
| 3mm | 3244.94 | 1.0000x | baseline-only |
| atax | 27.77 | 1.0000x | baseline-only |
| bicg | 32.05 | 1.0000x | baseline-only |
| doitgen | 556.02 | 1.0000x | baseline-only |
| mvt | 41.08 | 1.0000x | baseline-only |
| cholesky | 30445.95 | 1.0000x | baseline-only |
| durbin | 11.94 | 1.0000x | baseline-only |
| gramschmidt | 10256.91 | 1.0000x | baseline-only |
| ludcmp | 34893.87 | 1.0000x | baseline-only after timeout fix |
| lu | 34532.30 | 1.0000x | baseline-only after timeout fix |
| trisolv | 17.23 | 1.0000x | baseline-only |
| deriche | 303.04 | 1.0000x | baseline-only |
| floyd-warshall | 25467.89 | 1.0000x | baseline-only |
| nussinov | 5829.35 | 1.0000x | baseline-only |
| adi | 21366.07 | 1.0000x | baseline-only |
| fdtd-2d | 5289.04 | 1.0000x | baseline-only |
| heat-3d | 3959.03 | 1.0000x | baseline-only |
| jacobi-1d | 7.69 | 1.0000x | baseline-only |
| jacobi-2d | 1768.92 | 1.0000x | baseline-only |
| seidel-2d | 19715.74 | 1.0000x | baseline-only |

每个任务的原始日志和状态表在 `logs_polybench_full_20260722/`，两个 timeout 修复后的重跑日志在 `logs_polybench_retry_20260722/`。批量入口现在支持 `ROUNDS`、`RUNS`、`PIN_CPU`、`QUICK_CHECK`、`CONCURRENCY` 和 `LOGDIR`，单个任务失败不会中止其余任务。

## 真实 LLM 优化结果（2026-07-22 下午批次）

本机 DeepSeek API 在本批次恢复可用（此前批次全部 connection error），因此下面是本项目**第一批本地真实 LLM 驱动的优化结果**。全部数值均为 `[最终确认] 交替测量 baseline/best` 后的确认值，不使用候选搜索阶段的单次峰值；correctness 全部通过（PolyBench numeric/dump 模式）。

### 结果总表

| benchmark | 位置 | 预算 | 基线 (ms) | 确认加速比 | IQR | 关键动作 |
|---|---|---|---:|---:|---|---|
| gramschmidt | 本地 | rounds=5, runs=3 | 10370.24 | **18.1969x** | [17.3705, 18.3002] | rewrite_source：`restrict` 指针 + VLA 改静态数组；叠加 flags |
| correlation | 本地 | rounds=5, runs=3 | 5526.41 | **13.0127x** | [12.8754, 13.2101] | rewrite_source；`-vectorize-memory-check-threshold=256` |
| 3mm | 远程 | rounds=3, runs=3 | 9428.59 | **9.8424x** | [9.4979, 9.8873] | rewrite_source：循环交换 i-j-k → i-k-j；`--dse-memoryssa-defs-per-block-limit=80000` |
| 3mm | 本地 | rounds=3, runs=3 | 3712.51 | 3.9592x | — | rewrite_source；`-partial-unrolling-threshold=128` |
| floyd-warshall | 本地 | rounds=3, runs=3 | 16012.06 | 1.9004x | [1.8927, 1.9088] | rewrite_source：cache blocking |
| nussinov | 本地 | rounds=3, runs=3 | 5340.92 | 1.0541x | — | 仅 try_flags |
| jacobi-1d | 本地 | rounds=5, runs=3 | 6.61 | 1.0122x | [0.9365, 1.0404] | rewrite_source 尝试；确认值被噪声主导 |
| seidel-2d | 本地 | rounds=5, runs=3 | 21379.59 | 1.0029x | [1.0018, 1.0081] | 仅 try_flags，`-slp-max-vf=32` |

### 三个可复现的观察

**1. 大幅加速几乎全部来自 `rewrite_source`，不是参数调优。** 表中 >1.9x 的四项全部触发了源码重写；只走 `try_flags` 的项（nussinov、seidel-2d）确认值都在 1.06x 以内。gramschmidt 步骤 3 的纯 flag 组合（`-vectorize-scev-check-threshold=8 -licm-max-num-uses-traversed=16`）也只有 17.2x 中的一部分，主体收益来自重写。这解释了本文件上半部分 `rounds=1` 批次为何全是 `1.0000x`：预算不足以走到重写阶段。

**2. 重写的收益有明确的硬件计数器证据。** 远程 3mm 的循环交换使 IPC 从 0.79 升到 2.33、LLC miss 从 22.2% 降到 0.6%，与 9.84x 的确认加速比一致，不是测量伪影。

**3. 同一 kernel 的多次独立运行结果差异很大。** 本地和远程各跑一次 3mm（同为 rounds=3/runs=3），确认值分别是 3.96x 和 9.84x——LLM 在本地选择了 unroll 方向的重写，在远程选择了循环交换。这说明单次结果不能代表方法上限，论文主表必须使用多次独立重复的分布而非单点值。

短运行时 kernel 的确认测量噪声不可忽略：jacobi-1d 基线仅 6.61 ms，`base_cv=9.3%`，其 IQR [0.9365, 1.0404] 跨越 1.0，因此该项不应被解释为“有加速”。

原始产物位于 `runs/2026-07-22_10-58-43_polybench_*/` 与 `runs/2026-07-22_11-46-20_polybench_*/`（本地）、远程 `runs/2026-07-22_09-53-37_polybench_3mm/`，每个目录包含 `full.log`、`results.json`、`llm_calls.jsonl`、`outputs/*_agent_results.json` 和 `outputs/snapshots/`。

## SPEC CPU2017 多 TU 链路修复与首次 C++ 调优实验

远程 `SPEC_multitu_root/{deepsjeng_r,leela_r,namd_r}/build_manifest.json` 中的 workload `cwd`/`argv` 硬编码了本机路径 `/home/hanning/accelerate/comet/...`，该路径在远程不存在，导致这三个 C++ benchmark 的 multi-TU 实验链路在远程完全无法运行。`scripts/gen_spec_multitu.py` 本身用 `PROJECT_ROOT = Path(__file__).resolve().parents[1]` 推导路径，是正确的；问题出在 manifest 是在本机生成后同步过去的。在远程重新执行 `gen_spec_multitu.py --only deepsjeng_r` 后路径已修正为 `/home/hanning/comet/...`，leela_r 与 namd_r 需要同样处理。

修复后 deepsjeng_r 首次跑通完整 build → correctness → 配对计时链路：

| 项目 | 值 |
|---|---|
| workload | test |
| correctness | pass (numeric, epsilon 1e-4) |
| baseline median | 13.8415 s |
| candidate median | 13.8920 s |
| median paired speedup | 0.9929x |
| bootstrap CI95 | [0.9748, 1.0073] |
| candidate flags | `-mllvm -licm-max-num-uses-traversed=32 -mllvm -slp-threshold=10` |

置信区间跨越 1.0，因此这两个参数对 deepsjeng_r **没有可测量的效果**。这符合预期：这两个 flag 是从 3mm/gramschmidt 等 memory-bound 数值 kernel 的证据推导出来的，而 deepsjeng_r 是分支密集的棋类搜索引擎，瓶颈性质不同。本次实验的价值在于验证修复后的远程 multi-TU 链路可用，为后续按各 benchmark 自身 pass/perf 证据做 LLM 调优提供了基础。

注意 `run_manifest_experiment.py` 的 `--candidate-flag` 是逐个透传给 clang 的，LLVM 调试参数必须写成两个独立的 `--candidate-flag=-mllvm --candidate-flag=-licm-...=32`；直接写 `--candidate-flag=--licm-...=32` 会得到 `unknown argument` 并以 `candidate_build_failed` 结束。

结果 JSON：远程 `results/deepsjeng_r_manifest_test/experiment.json`。

## 后续（2026-07-22 晚间）：上报纪律修复 + 编译器反馈消融

本节之后的工作记录在 **`docs/ABLATION_RESULTS_20260722.md`**，要点：

- 新增 `--no-compiler-feedback` 消融条件（条件 B），屏蔽 pass remarks、missed
  transformation、pass 清单/purpose/status、IR 与 pass graph、`opt-21` 发现的
  debug 参数、LLM 审计阶段、perf 硬件反馈与 profile 热点重定向。屏蔽由
  `tests/test_ablation_no_feedback.py` 的标记串泄漏测试强制。
- 修复**上报纪律**：此前 `confirmed if ok else best_speedup` 会在「确认为回归」
  和「确认没跑成」两种情况下都发布一个未成立的数字。现由
  `decide_final_result()` 闸门统一处理，确认 < 1.0 一律回滚到纯 -O3 并记
  1.000x。这正是本文档 3mm 一节所记 **1.0492x 单次 vs 0.9886x 确认** 的教训。
- 3mm 的 LLM 源码重写（循环交换）做过**独立金标准正确性复核**：880,000 个
  dump 值逐位相同，最大相对误差 0.000e+00。

## 当前限制

远程代码版本尚未包含本机新增的 `--quick-check`/`pass-analysis` 代码；批量源码同步被安全策略阻止。远程本轮因此只执行了基线和正确性 smoke，未将其误报为 DeepSeek 优化结果。

> 更新：该限制已于 2026-07-22 晚间解除。远端源码已同步至与本机一致（`optimize.py`、
> `src/`、`tests/`、`skills/`、`scripts/`），远端 `configs/config.yaml` 保持
> `/usr/bin/*-21` 未被覆盖，远端测试 157 passed (10 skipped)。远端 DeepSeek 调用
> 已实际跑通完整 agent 流程。

远程一个遗留的 `lbm_r --rounds 5` 暴力任务运行约 13 分钟后被终止；lbm_r 已属于明确跳过范围，部分输出不计入结果。停止说明保存在远程 `quick_remote_20260722/INVALID_LBM_STOP.md`。
