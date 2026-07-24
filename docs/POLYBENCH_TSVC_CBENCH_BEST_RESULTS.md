# PolyBench、TSVC 与 cBench 历史最佳实验数据

整理日期：2026-07-21  
项目目录：`/home/hanning/accelerate/comet`

## 1. 统计口径

- 基线统一指各次实验自己记录的编译器 `-O3` baseline；`speedup = baseline runtime / optimized runtime`，数值越大越好。
- “历史 best”取当前项目中可追溯的完整汇总、最终日志或 `results.json` 中，同一 benchmark 最高的已报告结果。
- 表中的“估算 best runtime”由 `baseline / speedup` 计算，仅用于帮助阅读。
- 2026-07 的部分 PolyBench 日志同时提供搜索 best 和交替复测；复测值单列，不用它覆盖原日志的搜索 best。
- 不把普通参数网格中的单次峰值当成最终结果。例如 cBench `bzip2_encode` 曾单次筛到 1.169x，但没有完成交替确认，因此不计入正式历史 best。
- 不同批次的编译器、机器负载、运行次数、数据规模和搜索轮数并不完全一致。因此下文的“历史记录包络”适合做项目内部回顾，不应直接当作同一次、同配置的论文主表。

## 2. 总览

| 数据集 | 清单规模 | 有最终结果 | 历史最好效果 | 汇总状态 |
|---|---:|---:|---|---|
| PolyBench | 30 | 30 | `gramschmidt` 22.6657x | 完整；跨多批次取每项历史最高值 |
| TSVC | 151 | 99 | 暂无加速项 | 99 项已完成但均未超过 baseline；14 项中断、38 项未开始 |
| cBench | 19 | 0 | `bzip2_encode` 1.059x（中间 checkpoint） | 尚无完整最终结果；只有未完成试跑 |

PolyBench 30 项历史记录包络的几何平均为 **3.9559x**，其中 20 项达到至少 2x，29 项超过 1.05x。这个几何平均由不同批次的逐项最高值拼成，只表示历史上限。项目中已有的同批次完整 30 项实验是 2026-06-23 至 2026-06-24 的 **3.187x geomean**。

## 3. PolyBench 历史最佳结果

| Benchmark | 对应 baseline (ms) | 历史搜索 best | 提升 | 估算 best runtime (ms) | 来源批次 | 交替复测 |
|---|---:|---:|---:|---:|---|---:|
| 2mm | 2143.00 | 7.3580x | +635.8% | 291.25 | 2026-06 report | — |
| 3mm | 10280.55 | 11.3481x | +1034.8% | 905.93 | 2026-07 log | 8.5125x |
| adi | 12200.39 | 1.1264x | +12.6% | 10831.31 | 2026-06 JSON | — |
| atax | 7.00 | 2.0260x | +102.6% | 3.46 | 2026-06 report | — |
| bicg | 10.74 | 3.8082x | +280.8% | 2.82 | 2026-06 JSON | — |
| cholesky | 1444.69 | 1.2504x | +25.0% | 1155.38 | 2026-07 log | 1.1998x |
| correlation | 8528.40 | 18.6027x | +1760.3% | 458.45 | 2026-07 log | 18.2380x |
| covariance | 9545.75 | 18.5547x | +1755.5% | 514.47 | 2026-07 log | 17.2119x |
| deriche | 311.00 | 2.2210x | +122.1% | 140.03 | 2026-06 report | — |
| doitgen | 621.62 | 8.2168x | +721.7% | 75.65 | 2026-06 JSON | — |
| durbin | 3.15 | 3.8940x | +289.4% | 0.81 | 2026-06 report | — |
| fdtd-2d | 2253.00 | 1.8500x | +85.0% | 1217.84 | 2026-06 report | — |
| floyd-warshall | 27254.86 | 2.1260x | +112.6% | 12819.78 | 2026-07 log | 2.1111x |
| gemm | 423.00 | 1.3520x | +35.2% | 312.87 | 2026-06 report | — |
| gemver | 41.11 | 4.0458x | +304.6% | 10.16 | 2026-07 log | 3.9185x |
| gesummv | 4.77 | 2.5569x | +155.7% | 1.87 | 2026-07 log | 2.4705x |
| gramschmidt | 12025.97 | **22.6657x** | +2166.6% | 530.58 | 2026-06 JSON | — |
| heat-3d | 19189.94 | 1.7346x | +73.5% | 11062.91 | 2026-06 JSON | — |
| jacobi-1d | 0.47 | 1.0710x | +7.1% | 0.44 | 2026-06 report | — |
| jacobi-2d | 8956.79 | 1.2680x | +26.8% | 7063.86 | 2026-06 JSON | — |
| lu | 9970.76 | 1.8191x | +81.9% | 5481.15 | 2026-07 log | 1.8311x |
| ludcmp | 5347.00 | 4.5250x | +352.5% | 1181.66 | 2026-06 report | — |
| mvt | 31.29 | 4.5353x | +353.5% | 6.90 | 2026-07 log | 4.6583x |
| nussinov | 10980.96 | 14.5556x | +1355.6% | 754.41 | 2026-07 log | 14.2713x |
| seidel-2d | 20639.00 | 1.0270x | +2.7% | 20096.40 | 2026-06 report | — |
| symm | 5473.28 | 11.0606x | +1006.1% | 494.84 | 2026-07 log | 10.6879x |
| syr2k | 7399.05 | 8.8736x | +787.4% | 833.83 | 2026-07 log | 8.8767x |
| syrk | 1734.82 | 8.0946x | +709.5% | 214.32 | 2026-06 JSON | — |
| trisolv | 3.69 | 1.8180x | +81.8% | 2.03 | 2026-06 report | — |
| trmm | 3003.75 | 15.6897x | +1469.0% | 191.45 | 2026-06 JSON | — |

### 3.1 最高的几项

| 排名 | Benchmark | 历史 best | 主要优化信息 |
|---:|---|---:|---|
| 1 | gramschmidt | 22.6657x | 源码分块改善局部性，并使用 `-licm-max-num-uses-traversed=64` |
| 2 | correlation | 18.6027x | 循环交换与 tiling，配合 `--slp-max-store-lookup=-1`；交替复测 18.2380x |
| 3 | covariance | 18.5547x | 源码访问模式重写，配合 `-slp-threshold=10`；交替复测 17.2119x |
| 4 | trmm | 15.6897x | 源码优化，配合 SLP/LICM 参数组合 |
| 5 | nussinov | 14.5556x | 对 `j/k` 循环做 tiling；交替复测 14.2713x |

### 3.2 PolyBench 数据来源

- `2026-06 report`：[POLYBENCH_REPORT.md](../POLYBENCH_REPORT.md)，一次完整的 30 项实验，配置为每项 9 rounds、3 并发，整套 geomean 3.187x。
- `2026-06 JSON`：本地 [`runs/`](../runs/) 下各次运行的 `results.json`。其中 gramschmidt、trmm、doitgen、syrk、bicg 和 adi 的历史记录来自 2026-06-28；heat-3d 与 jacobi-2d 来自 [`results/polybench_optimization_results.json`](../results/polybench_optimization_results.json)。
- `2026-07 log`：本地 [`logs/`](../logs/) 的逐项最终汇总及交替复测；整体状态可参考 [POLYBENCH_RUN_STATUS.md](POLYBENCH_RUN_STATUS.md)。

需要注意：部分 2026-07 日志内嵌的绝对路径仍是旧位置 `/home/hanning/comet/...`，该旧目录当前不存在。上表数值可以由本仓库日志追溯，但相应旧路径下的优化源码和结果 JSON 不能直接访问。

## 4. TSVC 历史结果

TSVC manifest 共有 151 项，当前 [`logs_tsvc/`](../logs_tsvc/) 中有 113 个日志，其中 99 项已经形成最终汇总。

### 有加速的历史结果（仅显示最终 speedup > 1.0x）

**暂无符合条件的 TSVC 项目。**

已经完成的 99 项最终都回退到 baseline，因此按“只展示有加速结果”的口径全部省略。其余记录中，14 项因 IR 提取、编译或基线计时失败而没有最终 speedup，另有 38 项尚无日志。

日志里的参数搜索过程虽然出现过短时计时波动，但这些候选没有通过最终接受或确认条件，不能作为正式加速结果列入表格。

## 5. cBench 历史结果

cBench manifest 当前有 19 项，但项目中没有 `logs_cbench/` 完整结果，也没有任何 cBench `results.json`。现有证据仅来自 2026-07-21 的 `bzip2_encode` 多次未完成试跑。

| Benchmark | Baseline | 当前可保留的最好 checkpoint | 交替确认 | 最终状态 |
|---|---:|---:|---:|---|
| bzip2_encode | 94.08 ms | 1.059x，`--unroll-max-upperbound=128` | 1.065x，IQR [1.059, 1.079] | 运行停在 step 3/5，无最终 summary/JSON |
| 其余 18 项 | — | — | — | 未开始或没有结果文件 |

证据日志是 [`runs/2026-07-21_16-23-09_cbench_bzip2_encode/full.log`](../runs/2026-07-21_16-23-09_cbench_bzip2_encode/full.log)。较早一次未完成运行在参数网格里出现过 1.169x 单次值（98.20 ms → 84.0 ms），但运行在 step 1 内终止、没有交替确认，故只视为噪声敏感的候选，不列为正式历史 best。

因此，cBench 当前最准确的表述是：**尚无完成的历史最终效果；`bzip2_encode` 有一个 1.059x 的已接受中间 checkpoint，交替测量约 1.065x。**

## 6. 可直接引用的结论

- PolyBench 的历史最高单项是 `gramschmidt` 22.6657x；其次是 `correlation` 18.6027x、`covariance` 18.5547x、`trmm` 15.6897x 和 `nussinov` 14.5556x。
- PolyBench 唯一明确记录的完整同批次 30 项 geomean 是 3.187x；跨批次逐项取最高值形成的历史记录包络 geomean 是 3.9559x。
- TSVC 已完成的 99 项没有产生被最终接受的加速，因此不展示具体项目；其余 52 项尚未形成最终结果。
- cBench 尚无完整最终数据，不能给出 suite geomean；目前只有 `bzip2_encode` 的 1.059x 中间结果。
