# 消融实验完整结果汇总 (2026-07-27，持续更新)

覆盖49个程序（30 PolyBench + 19 cBench），全部在两台DGX机器上运行（不含oracle4）。测量统一采用**4次取样去掉最高最低取中间2次均值**的trimmed-mean方式，并发worker之间通过`taskset --pin-cpu`隔离到独立物理核心。

## 进度

| 条件 | 说明 | 已完成/总数 |
|---|---|---|
| ① | 只改代码（无编译反馈） | 49/49 ✅ |
| ② | 改代码+调参（无编译反馈） | 49/49 ✅ |
| ③ | 完整系统（代码+调参+编译反馈，仅4个重跑） | 4/49 🔄 进行中 |
| ④ | 只调参不改代码（无编译反馈） | 19/49 🔄 进行中 |
| OC | OpenCode+DeepSeek baseline | 49/49 ✅ |
| PO | AutoPass风格pass顺序搜索（已修复DUMP_ARRAYS正确性bug） | 22/49 🔄 进行中 |

> **重要修复记录**：
> 1. `passorder_autopass_style`（AutoPass风格pass顺序搜索）此前因为把`-DPOLYBENCH_TIME`构建的
>    计时输出误当成正确性比较对象（该宏下只打印耗时，不打印真实数组），暂停过一次；已修复
>    （改为用独立的`-DPOLYBENCH_DUMP_ARRAYS`构建做正确性校验），修复后已重新跑，本报告的
>    passorder数据全部来自修复后的结果。
> 2. `automotive_susan_smoothing`（cBench）此前发现comet的热点函数识别bug：`select_hotspot_targets`
>    纯静态调用图分析选中了从未被"-s"这次调用执行过的`susan_thin`（只在edges模式可达），
>    导致agent一直在改死代码；已修复（新增gdb运行时校验，逐个排除未执行候选并回落到真正的
>    热点函数）。**但④和passorder条件里已跑完的susan_smoothing相关任务是否用的是修复前还是
>    修复后的代码，取决于具体跑的时间点，本报告未逐一区分，如需引用请先核实。**

## ⚠️ 待解读的现象：PO（passorder）修复正确性bug后，"confirmed"的结果普遍比baseline慢很多

修复了`-DPOLYBENCH_TIME`误判正确性的bug之后，PO条件目前12个"confirmed"（正确性真正通过）的结果，
加速比全部在**0.205x～0.718x**之间（也就是比-O3 baseline慢**39%到390%**），没有一个超过1.0x。
这**不是bug**（正确性已经用DUMP_ARRAYS真实校验过），而是这个harness本身设计上的一个真实局限：
kernel部分用`-O1 -Xclang -disable-llvm-passes`拿到未优化IR后，只套用一个22个pass的自定义目录
（`CANONICAL_PASSES`），远少于`-O3`真实跑的完整pass battery，而且最后codegen阶段是
`clang -c kernel_opt.ll`不带任何`-O`标志（默认走O0档codegen）。换句话说，这个baseline目前呈现的是
"一个远比-O3简陋的pass子集+O0 codegen，普遍不如-O3"，而不是真正在验证"pass顺序本身是否重要"这个
问题——这可能是个值得写进论文的真实发现，也可能需要你决定是否要改进harness（比如让codegen也走
`-O3`档、或者扩充pass目录）后再判断这是否是最终结论。

## ①②③④OC 总体统计（区分 correct-但无提升 / incorrect / 有效加速）

| 条件 | n | correct且有加速(>1.0x) | correct但≤1.0x | incorrect(正确性失败) | baseline_only(未找到候选) |
|---|---|---|---|---|---|
| ① 只改代码（无编译反馈） | 49 | 40 | 1 | 0 | 8 |
| ② 改代码+调参（无编译反馈） | 49 | 42 | 5 | 0 | 2 |
| ③ 完整系统（代码+调参+编译反馈，仅4个重跑） | 4 | 2 | 1 | 0 | 1 |
| ④ 只调参不改代码（无编译反馈） | 19 | 13 | 1 | 0 | 5 |
| OC OpenCode+DeepSeek baseline | 49 | 11 | 12 | 26 | 0 |

## 加速比统计（排除incorrect的失真1.0x）

| 条件 | n(有效) | 几何均值 | 中位数 | 均值 | 范围 |
|---|---|---|---|---|---|
| ① 只改代码（无编译反馈） | 49 | 1.919x | 1.242x | 4.498x | 0.950x – 98.854x |
| ② 改代码+调参（无编译反馈） | 49 | 1.570x | 1.109x | 2.201x | 0.992x – 15.071x |
| ③ 完整系统（代码+调参+编译反馈，仅4个重跑） | 4 | 2.067x | 1.163x | 4.278x | 0.999x – 13.790x |
| ④ 只调参不改代码（无编译反馈） | 19 | 1.041x | 1.020x | 1.042x | 0.996x – 1.245x |
| OC OpenCode+DeepSeek baseline | 23 | 1.006x | 0.999x | 1.006x | 0.953x – 1.164x |
| PO AutoPass风格pass顺序搜索（已修复DUMP_ARRAYS正确性bug） | 17 | 0.506x | 0.471x | 0.583x | 0.205x – 1.000x |

> **注**：③（完整系统）目前只有4个样本，是之前误跑在oracle4上的4个任务
> （correlation、trisolv、network_patricia、security_sha）在dgx上的重跑；其余45个完整系统结果
> 复用此前独立跑完的49任务全量运行（未采用本次trimmed-mean-of-4测量法，口径不完全一致）。
>
> ④（只调参不改代码）和PO目前仍在跑，本报告只包含已完成的部分。

## ④ 只调参不改代码 —— 详细任务表

| 程序 | 数据集 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|---|
| 2mm | polybench | 1192.420 | 1.276x | **1.245x** | confirmed | True | 3 | dgx-spark-a-1 |
| durbin | polybench | 2.180 | 1.050x | **1.138x** | confirmed | True | 3 | dgx-spark-a-2 |
| syrk | polybench | 1160.180 | 1.156x | **1.081x** | confirmed | True | 3 | dgx-spark-b-0 |
| gemm | polybench | 195.900 | 1.110x | **1.068x** | confirmed | True | 3 | dgx-spark-b-1 |
| mvt | polybench | 20.730 | 1.072x | **1.066x** | confirmed | True | 3 | dgx-spark-a-2 |
| doitgen | polybench | 240.890 | 1.046x | **1.054x** | confirmed | True | 3 | dgx-spark-b-2 |
| syr2k | polybench | 1126.160 | 1.051x | **1.044x** | confirmed | True | 3 | dgx-spark-b-2 |
| gemver | polybench | 21.430 | 1.032x | **1.037x** | confirmed | True | 3 | dgx-spark-b-2 |
| trisolv | polybench | 9.300 | 1.127x | **1.027x** | confirmed | False | 3 | dgx-spark-a-1 |
| deriche | polybench | 132.080 | 1.013x | **1.020x** | confirmed | True | 3 | dgx-spark-a-1 |
| correlation | polybench | 1479.630 | 1.017x | **1.018x** | confirmed | True | 3 | dgx-spark-a-1 |
| symm | polybench | 898.860 | 1.019x | **1.009x** | confirmed | True | 3 | dgx-spark-a-2 |
| trmm | polybench | 524.190 | 1.007x | **1.001x** | confirmed | False | 3 | dgx-spark-b-1 |
| covariance | polybench | 4806.280 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-0 |
| 3mm | polybench | 3484.700 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-0 |
| bicg | polybench | 23.890 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-2 |
| atax | polybench | 15.420 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-2 |
| ludcmp | polybench | 6877.130 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-2 |
| gesummv | polybench | 23.290 | 1.026x | **0.996x** | confirmed | False | 3 | dgx-spark-b-0 |

## PO AutoPass风格pass顺序搜索（修复后）—— 详细任务表

（已完成22个：confirmed=12，incorrect/compile_failed/confirm_failed=10）

| 程序 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|
| PolyBenchC_no_rag/medley/deriche/deriche.c | 139.286 | 1.000x | **0.718x** | confirmed | False | 3 | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/blas/syr2k/syr2k.c | 1121.012 | 1.000x | **0.699x** | confirmed | False | 3 | dgx-spark-b-2 |
| PolyBenchC_no_rag/linear-algebra/solvers/trisolv/trisolv.c | 8.776 | 1.000x | **0.521x** | confirmed | False | 3 | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/blas/gemver/gemver.c | 21.598 | 1.000x | **0.471x** | confirmed | False | 3 | dgx-spark-b-2 |
| PolyBenchC_no_rag/linear-algebra/blas/symm/symm.c | 974.677 | 1.000x | **0.399x** | confirmed | False | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/gramschmidt/gramschmidt.c | 1522.867 | 1.000x | **0.398x** | confirmed | False | 3 | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c | 6063.830 | 1.000x | **0.336x** | confirmed | False | 3 | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c | 7132.401 | 1.000x | **0.323x** | confirmed | False | 3 | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/solvers/ludcmp/ludcmp.c | 7608.330 | 1.000x | **0.308x** | confirmed | False | 3 | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/durbin/durbin.c | 1.953 | 1.000x | **0.275x** | confirmed | False | 3 | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/3mm/3mm.c | 1987.678 | 1.000x | **0.255x** | confirmed | False | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/2mm/2mm.c | 1000.043 | 1.000x | **0.205x** | confirmed | False | 3 | dgx-spark-b-2 |
| PolyBenchC_no_rag/datamining/correlation/correlation.c | 1438.623 | 1.000x | N/A(incorrect) | incorrect | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/datamining/covariance/covariance.c | 4823.404 | 1.000x | N/A(compile_failed) | compile_failed | False | None | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/blas/gemm/gemm.c | 145.744 | 1.000x | N/A(compile_failed) | compile_failed | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/blas/gesummv/gesummv.c | 15.844 | 1.000x | N/A(incorrect) | incorrect | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/blas/syrk/syrk.c | 381.397 | 1.000x | N/A(compile_failed) | compile_failed | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/blas/trmm/trmm.c | 520.221 | 1.000x | N/A(incorrect) | incorrect | False | None | dgx-spark-a-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/atax/atax.c | 17.246 | 1.000x | N/A(incorrect) | incorrect | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/kernels/bicg/bicg.c | 26.774 | 1.000x | N/A(compile_failed) | compile_failed | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/kernels/doitgen/doitgen.c | 241.844 | 1.000x | N/A(incorrect) | incorrect | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/kernels/mvt/mvt.c | 36.924 | 1.000x | N/A(compile_failed) | compile_failed | False | None | dgx-spark-a-2 |

## 逐程序跨条件对比矩阵

同一个程序在①②③④OC/PO几个条件下的加速比对比。④/PO列若还没跑完会显示`-`。

| 程序(base_id) | ① 只改代码 | ② 代码+调参无反馈 | ③ 完整系统 | ④ 只调参 | OC baseline | PO pass顺序 |
|---|---|---|---|---|---|---|
| cb001 | 1.000x | 1.331x | - | - | 0.997x | - |
| cb002 | 1.174x | 1.260x | - | - | 0.992x | - |
| cb003 | 1.046x | 1.381x | - | - | 0.998x | - |
| cb004 | 1.546x | 1.004x | - | - | 1.001x | - |
| cb005 | 98.854x | 0.998x | - | - | 0.997x | - |
| cb006 | 1.016x | 0.997x | - | - | 1.007x | - |
| cb007 | 1.155x | 1.048x | - | - | 1.021x | - |
| cb008 | 1.000x | 0.996x | - | - | 1.002x | - |
| cb009 | 1.000x | 0.992x | - | - | 1.006x | - |
| cb010 | 1.000x | 1.097x | - | - | 1.031x | - |
| cb011 | 1.062x | 1.454x | - | - | 0.995x | - |
| cb012 | 1.208x | 1.038x | 1.325x | - | 0.953x | - |
| cb013 | 1.037x | 1.066x | - | - | 0.979x | - |
| cb014 | 1.002x | 1.006x | - | - | 1.011x | - |
| cb015 | 1.000x | 1.016x | - | - | 0.998x | - |
| cb016 | 1.000x | 1.000x | 1.000x | - | incorrect | - |
| cb017 | 1.000x | 1.151x | - | - | 0.992x | - |
| cb018 | 1.000x | 1.191x | - | - | 0.978x | - |
| cb019 | 0.950x | 1.044x | - | - | 1.003x | - |
| pb001 | 11.250x | 9.223x | 13.790x | 1.018x | incorrect | incorrect |
| pb002 | 14.316x | 15.071x | - | 1.000x | incorrect | compile_failed |
| pb003 | 1.295x | 1.015x | - | 1.068x | incorrect | compile_failed |
| pb004 | 1.588x | 1.174x | - | 1.037x | incorrect | 0.471x |
| pb005 | 1.385x | 1.402x | - | 0.996x | incorrect | incorrect |
| pb006 | 4.808x | 4.381x | - | 1.009x | incorrect | 0.399x |
| pb007 | 4.029x | 2.995x | - | 1.044x | incorrect | 0.699x |
| pb008 | 2.527x | 1.100x | - | 1.081x | incorrect | compile_failed |
| pb009 | 6.680x | 7.414x | - | 1.001x | incorrect | incorrect |
| pb010 | 7.037x | 5.480x | - | 1.245x | incorrect | 0.205x |
| pb011 | 5.877x | 4.924x | - | 1.000x | incorrect | 0.255x |
| pb012 | 1.069x | 1.091x | - | 1.000x | 0.976x | incorrect |
| pb013 | 2.069x | 1.720x | - | 1.000x | 0.999x | compile_failed |
| pb014 | 4.054x | 4.013x | - | 1.054x | incorrect | incorrect |
| pb015 | 1.242x | 1.182x | - | 1.066x | incorrect | compile_failed |
| pb016 | 1.096x | 1.083x | - | - | incorrect | 0.336x |
| pb017 | 1.865x | 1.019x | - | 1.138x | 1.018x | 0.275x |
| pb018 | 5.552x | 5.988x | - | - | incorrect | 0.398x |
| pb019 | 1.309x | 1.078x | - | 1.000x | incorrect | 0.308x |
| pb020 | 1.194x | 1.191x | - | - | incorrect | 0.323x |
| pb021 | 1.100x | 1.153x | 0.999x | 1.027x | 1.025x | 0.521x |
| pb022 | 2.001x | 1.564x | - | 1.020x | incorrect | 0.718x |
| pb023 | 7.877x | 3.781x | - | - | incorrect | - |
| pb024 | 1.305x | 1.109x | - | - | incorrect | - |
| pb025 | 3.771x | 1.510x | - | - | incorrect | - |
| pb026 | 1.200x | 0.999x | - | - | incorrect | - |
| pb027 | 1.446x | 1.022x | - | - | incorrect | - |
| pb028 | 1.086x | 1.058x | - | - | 1.164x | - |
| pb029 | 1.339x | 1.000x | - | - | incorrect | - |
| pb030 | 1.003x | 1.017x | - | - | incorrect | - |
