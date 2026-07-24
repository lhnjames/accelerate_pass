> ⚠️ **已作废（SUPERSEDED，2026-07-24）**：本文件的数字来自 **quick-check + rounds=3** 的浅搜索，该模式砍掉了 exhaustive flag 补充搜索，会低估参数通道的效果。已按用户要求删除对应远端运行数据，并以 **rounds=5、无 quick-check** 的完整深度消融重跑取代。最终数字见 r5 结果目录 (`cbench_r5_logs/`、`polybench_r5_logs/`) 与后续新报告。以下内容仅保留方法学与设计说明，数字不作数。

# CBench 编译器反馈消融（Full vs No-compiler-feedback）— 2026-07-23

与 PolyBench 消融（`docs/ABLATION_SNAPSHOT_20260723.md`）**同一套方法、同一份代码、
同一条纪律**，只是把目标从 PolyBench 换成 cBench 的 19 个 kernel。回答同一个问题：
**编译器反馈能不能帮 LLM 在真实工作负载（非规整数值 kernel）上找到更好的优化？**

> 状态：驱动已支持 cBench（`--dataset cbench` + `--programs-file`），单程序冒烟
> 已在远端跑通（telecom_crc32：dataset 正确检测、证据收集、正确性模式、最终确认
> 全链路可用）。全量 38 cell 扫描见下。

## 1. 配置

| 项 | 值 |
|---|---|
| 主机 | `ubuntu@132.145.22.86`，4 核 aarch64 Neoverse-N1 |
| 编译器 | `/usr/bin/clang-21`（21.1.8） |
| 程序 | 19 个 cBench kernel（`CBench_shim_root/manifest.txt`） |
| 条件 | full, no_feedback |
| seed | 1（单 seed，与 PolyBench 决定一致） |
| 每 cell 预算 | `--rounds 3 --quick-check` |
| 确认 | `--runs 5`，交替 baseline/candidate + 外部墙钟 |
| 绑核 / 并发 | `--pin-cpu 2` / 串行 |
| cells | 19 × 2 × 1 = 38 |

条件定义、屏蔽强制方式、回滚闸门与 PolyBench 消融**完全一致**——见
`docs/ABLATION_RESULTS_20260722.md` §1–§2，此处不重复。

## 2. cBench 相较 PolyBench 的注意点

- **很多 kernel 极快**（telecom_crc32 baseline 仅 1.49ms）。快程序计时噪声大——
  但 `iqr_excludes_one` 判据仍要求确认 IQR **整段** > 1.0 才算真增益，把噪声挡在外面。
  报告里对 baseline < 5ms 的程序会特别标注，其 1.0x 附近的"增益"需谨慎看待。
- **正确性模式更多样**：cBench 有 hash / numeric / exit_only 多种模式（susan_corners
  曾为 hash）。optimize.py 自动检测，最终确认沿用外部墙钟计时，不依赖程序 stdout。
- **热点重定向更常见**：cBench kernel 常是 wrapper，真正热点在 driver 文件里
  （crc32 冒烟就重定向到了 `crc32file`）。这在 no_feedback 条件下被屏蔽（profile
  热点属硬件反馈），是两个条件的一个真实差异来源。

## 2.1 已知排除

- **office_stringsearch2**：两个条件均在 ~4s 内失败，原因是**基线计时失败**
  （日志："基线计时失败，请检查编译配置"）。证据收集本身成功（remarks/pass
  graph/perf/正确性模式=exit_only/热点重定向到 local_strncmp+strsearch2 都正常），
  失败发生在 baseline 计时阶段。stringsearch 需要输入文件（字典/待搜索文本），
  该 shim 很可能未提供输入或工作目录不对，程序非零退出导致计时失败。**两条件
  对称失败，不偏向任何一方**——作为 shim 的数据依赖问题排除出对比，不计入统计。
  待后续单独修 shim（提供输入 + 工作目录）后补测。

## 3. 结果（扫描完成 2026-07-23）

18 个程序两条件全部完成（stringsearch2 排除，见 §2.1）。`final` = 正式确认加速比
（n=5 交替），`(RB)` = rolled_back_regression，`(base)` = baseline_only。

| program | full | no_feedback | 配对方向 |
|---|---:|---:|---|
| susan_edges | 1.00 (base) | **1.129** | nf 赢 |
| tiff2bw | **1.067** | 1.025 | full 略高 |
| crc32 | 1.046 | **1.064** | nf 略高 |
| rijndael_encode | **1.050** | 1.00 (base) | full 赢 |
| dijkstra | 1.00 (RB) | **1.031** | nf 赢 |
| bzip2_encode | **1.022** | 1.00 (RB) | full 赢 |
| susan_corners | **1.017** | 1.00 (base) | full 赢 |
| tiff2dither | 1.015 | 1.014 | ≈ |
| adpcm_c | 1.00 (base) | 1.009 | nf 略高 |
| adpcm_d | 1.00 (RB) | 1.003 | ≈ |
| susan_smoothing | 1.00 (RB) | 1.002 | ≈ |
| qsort1 | 1.00 (RB) | 1.00 (RB) | 平 |
| bzip2_decode | 1.00 (RB) | 1.00 (RB) | 平 |
| tiff2median | 1.00 (base) | 1.00 (RB) | 平 |
| tiff2rgba | 1.00 (RB) | 1.00 (RB) | 平 |
| patricia | 1.00 (RB) | 1.00 (RB) | 平 |
| rijndael_decode | 1.00 (RB) | 1.00 (RB) | 平 |
| sha | 1.00 (base) | 1.00 (base) | 平 |

### 汇总统计

| 指标 | full | no_feedback |
|---|---:|---:|
| 可用程序 | 18 | 18 |
| geomean（程序中位数） | **1.0118x** | **1.0148x** |
| 成功率 ≥1.0 | 100% | 100% |
| 严格增益 >1.01 | 33% | 28% |
| IQR 整段>1.0 的真增益 | 6/18 (33%) | 5/18 (28%) |
| rolled_back_regression | 8 | 7 |
| baseline_only | 4 | 3 |

**配对比较（18 对）**：
- geomean full = 1.0118x，no_feedback = 1.0148x
- 配对比值 full/no_feedback = **0.9970**，bootstrap **CI95 = [0.9984, 1.0003]**
- head-to-head：**full 赢 5，no_feedback 赢 6，平 7**

### 结论

1. **两条件统计上不可区分，且比 PolyBench 更接近。** 配对比值 CI95 [0.998, 1.000]
   极窄、紧贴 1.0，head-to-head 5:6:7 近乎完全平局。**"No-feedback ≈ Full" 在
   cBench 上更强地成立**——增益幅度小，条件差异被进一步压缩。

2. **真实工作负载对 LLM 优化近乎"免疫"。** geomean ≈1.01x，而 PolyBench ~3x。
   18 个程序里 15 个落在 rolled_back / baseline / ≈1.0——分支密集、指针追逐、
   加密/压缩/排序类代码在 -O3 之外几乎没有 LLM 能拿到的结构性优化空间。这是与
   PolyBench 规整数值 kernel 的**核心对比结论**：编译器反馈的价值（以及 LLM 优化
   本身的价值）高度依赖工作负载的规整程度。

3. **分歧双向且更碎**：full 赢 susan_corners/bzip2_encode/rijndael_encode，
   no_feedback 赢 susan_edges/dijkstra。反馈的作用因程序而异，不单调——与 PolyBench
   一致。

4. **回滚闸门大量生效**：15/36 cell 回滚到 1.0，把探索期单次假增益（含 1.49ms
   crc32 冒烟里的 1.0984x）全部挡回。真实工作负载噪声大，这条纪律在 cBench 上尤其
   关键。

### 与 PolyBench 消融对照

| | PolyBench (28/30 程序) | cBench (18/19 程序) |
|---|---|---|
| full geomean | ~3.0x | 1.012x |
| no_feedback geomean | ~3.0x | 1.015x |
| 配对比值 CI95 | [0.985, 1.009] | [0.998, 1.000] |
| head-to-head | 11:10 (平 2) | 5:6 (平 7) |
| 结论 | full≈no_feedback | full≈no_feedback（更强） |

两个 benchmark suite 给出**一致的主结论**：在相同 LLM/预算/测量下，**移除编译器
反馈几乎不改变 LLM 能达到的确认加速**。差别在于 cBench 的可优化空间本身就小得多。

## 4. 路径

| 内容 | 路径 |
|---|---|
| 逐 cell 结果 | `/home/hanning/comet/cbench_ablation_logs/results.jsonl` |
| 扫描主日志 | `/home/hanning/comet/cbench_ablation_logs/sweep_master.log` |
| 每 cell 日志 | `/home/hanning/comet/cbench_ablation_logs/<program>_<condition>_seed1.log` |
| 每 run 完整记录 | `/home/hanning/comet/runs/<ts>_cbench_<program>/` |
