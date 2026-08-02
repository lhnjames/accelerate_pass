# COMET 消融实验 — 全量精细数据快照

_生成时间：2026-07-31 17:31UTC_

本文件是**逐任务级**的原始数据落盘，用于论文表格与复核。所有数字都从队列 `state.json` 的任务→节点归属出发，回到该节点上对应的结果 JSON / 日志 JSON 块提取，不依赖任何中间汇总文件。

## 1. 测量环境

| 项 | 值 |
|---|---|
| 架构 | aarch64 |
| CPU | NVIDIA DGX Spark (GB10 Grace)，Cortex-X925 + Cortex-A725，20 核 |
| 节点 | `dgx-spark-a`、`dgx-spark-b`（各 1 个 worker slot，均 `--pin-cpu 2`） |
| 工具链 | Ubuntu clang 21.1.8 / opt-21 / llc-21，target `aarch64-unknown-linux-gnu` |
| baseline | `clang -O3`，六个条件共用同一条编译路径（已核验二进制 SHA256 一致） |
| dataset 宏 | PolyBench: `LARGE_DATASET`；cBench: shim 默认输入 |
| 任务队列 | `oracle4:8001`，state 落盘 `/home/hanning/comet_queue/state.json` |
| 每程序预算 | 条件 ①②③④/OC：9 步；PO：3 轮（R3，对齐论文主结果配置） |
| 最终确认 | 与 -O3 交替配对测量，`runs=3` |

## 2. 总体进度

| 条件 | done | running | pending | 合计 |
|---|---:|---:|---:|---:|
| ① rewrite-only | 50 | 0 | 1 | 51 |
| ② no-compiler-feedback | 50 | 0 | 1 | 51 |
| ③ full system | 5 | 0 | 1 | 6 |
| ④ params-only | 48 | 2 | 1 | 51 |
| OC | 49 | 0 | 2 | 51 |
| PO | 49 | 0 | 2 | 51 |
| **合计** | **251** | **2** | **8** | **261** |

未完成任务明细：

| 任务 | 状态 | 节点 | 程序 |
|---|---|---|---|
| `c4_pb030` | running（07-31 10:07 起） | dgx-spark-b-0 | seidel-2d |
| `c4_cb020` | running（07-31 16:49 起） | dgx-spark-a-0 | security_blowfish_encode |
| `oc_cb020` | pending（— 起） | — | security_blowfish_encode |
| `po_cb020` | pending（— 起） | — | security_blowfish_encode |
| `c1_cb021` | pending（— 起） | — | security_blowfish_decode |
| `c2_cb021` | pending（— 起） | — | security_blowfish_decode |
| `c3_cb021` | pending（— 起） | — | security_blowfish_decode |
| `c4_cb021` | pending（— 起） | — | security_blowfish_decode |
| `oc_cb021` | pending（— 起） | — | security_blowfish_decode |
| `po_cb021` | pending（— 起） | — | security_blowfish_decode |

## 3. 六条件汇总

| 条件 | n | geomean | 中位数 | 最小 | 最大 | >1.05 | 恰好 1.000 | <0.95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ① rewrite-only | 50 | **1.8786** | 1.2249 | 0.9498 | 98.8539 | 32 | 14 | 1 |
| ② no-compiler-feedback | 50 | **1.5519** | 1.0453 | 0.9979 | 15.0710 | 24 | 11 | 0 |
| ③ full system | 5 | **1.9151** | 1.2095 | 1.0000 | 13.7896 | 4 | 1 | 0 |
| ④ params-only | 48 | **1.0980** | 1.0337 | 0.9963 | 1.8909 | 20 | 7 | 0 |
| OC | 49 | **1.0030** | 1.0000 | 0.9533 | 1.1640 | 1 | 26 | 0 |
| PO | 49 | **1.0313** | 1.0010 | 0.6653 | 1.6012 | 10 | 1 | 5 |

`final_status` 分布（`baseline_only` = 预算跑完但没有任何改动通过最终确认）：

| 条件 | confirmed | baseline_only | 其它 |
|---|---:|---:|---|
| ① rewrite-only | 36 | 14 | — |
| ② no-compiler-feedback | 39 | 11 | — |
| ③ full system | 4 | 1 | — |
| ④ params-only | 41 | 7 | — |
| OC | 23 | 0 | {'incorrect': 26} |
| PO | 48 | 0 | {'incorrect': 1} |

> **`incorrect` 的含义**：最终产物未通过与 -O3 参考输出的比对，该任务加速比被强制置为 1.0000 后计入 geomean。
> OC 有 26 个、PO 有 1 个，详见 §7。因此 OC 的 geomean 1.0030 里有 26 个是"优化失败记 1.0"，
> 而非"agent 找不到优化"——两者的含义完全不同。

### 3.1 分 suite

| 条件 | PolyBench n / geomean | cBench n / geomean |
|---|---|---|
| ① rewrite-only | 30 / **2.4029** | 20 / **1.2986** |
| ② no-compiler-feedback | 30 / **2.0686** | 20 / **1.0084** |
| ③ full system | 2 / **3.8612** | 3 / **1.1999** |
| ④ params-only | 29 / **1.0932** | 19 / **1.1054** |
| OC | 30 / **1.0057** | 19 / **0.9988** |
| PO | 30 / **1.0024** | 19 / **1.0785** |
## 4. PO — AutoPass 复现（逐任务）

每任务 3 轮（R3），每轮 Reasoning Agent 产出 1 条 pass 顺序 + 参数组合，Evaluation Agent 严格 `t(P) < t(P*)` 才接受。`回退O3` = 三轮都没赢过 `default<O3>`，最终二进制就是 LLVM 自带 -O3 pipeline（此时确认值理论上应为 1.000）。

| 任务 | 程序 | baseline (ms) | 探索期 best | **确认值** | IQR | 显著 | 回退O3 | 接受/拒绝/编译失败 | #pass | #param | Score Agent 目标 (score) | 节点 | 用时(min) |
|---|---|---:|---:|---:|---|:--:|:--:|:--:|---:|---:|---|---|---:|
| `po_cb001` | automotive_qsort1 | 30.81 | 1.1845 | **0.9520** | [0.892, 1.074] |  |  | 1/2/0 | 15 | 6 | qsortx (51) | dgx-spark-a-0 | 19s |
| `po_cb002` | automotive_susan_corners | 15.90 | 1.1687 | **1.1645** | [0.857, 1.249] | ✓ |  | 2/1/0 | 20 | 5 | susan_corners (184) | dgx-spark-a-0 | 24s |
| `po_cb003` | automotive_susan_edges | 22.44 | 1.0050 | **0.9727** | [0.851, 1.053] |  |  | 1/1/1 | 20 | 4 | susan_corners (184) | dgx-spark-a-0 | 21s |
| `po_cb004` | automotive_susan_smoothing | 123.65 | 1.0000 | **1.0306** | [0.984, 1.043] | ✓ | ✓ | 0/0/3 | 1 | 0 | susan_corners (184) | dgx-spark-a-0 | 27s |
| `po_cb005` | bzip2_decode | 118.24 | 1.0189 | **0.9824** | [0.973, 0.994] |  |  | 1/1/1 | 24 | 9 | kernel_bzip2_decode (263) | dgx-spark-a-0 | 36s |
| `po_cb006` | bzip2_encode | 179.85 | 1.0138 | **1.0017** | [0.965, 1.005] | ✓ |  | 1/1/1 | 22 | 5 | kernel_bzip2_encode (263) | dgx-spark-a-0 | 38s |
| `po_cb007` | consumer_tiff2bw | 5.07 | 1.0000 | **1.4128** | [1.121, 1.584] | ✓ | ✓ | 0/0/3 | 1 | 0 | TIFFReadDirectory (305) | dgx-spark-a-0 | 20s |
| `po_cb008` | consumer_tiff2dither | 7.99 | 1.7036 | **0.9697** | [0.790, 1.250] |  |  | 2/0/1 | 22 | 8 | TIFFReadDirectory (305) | dgx-spark-a-0 | 28s |
| `po_cb009` | consumer_tiff2median | 2.15 | 1.7426 | **0.9996** | [0.986, 1.010] |  |  | 3/0/0 | 24 | 9 | TIFFReadDirectory (305) | dgx-spark-a-0 | 31s |
| `po_cb010` | consumer_tiff2rgba | 9.22 | 1.0000 | **1.1858** | [0.883, 1.217] | ✓ | ✓ | 0/3/0 | 1 | 0 | TIFFReadDirectory (305) | dgx-spark-a-0 | 33s |
| `po_cb011` | network_dijkstra | 2.14 | 1.0000 | **0.6653** | [0.322, 1.226] |  | ✓ | 0/0/3 | 1 | 0 | kernel_network_dijkstra (52) | dgx-spark-a-0 | 19s |
| `po_cb012` | network_patricia | 2.96 | 1.3821 | **1.5930** | [0.724, 1.845] | ✓ |  | 3/0/0 | 24 | 8 | kernel_network_patricia (96) | dgx-spark-a-0 | 18s |
| `po_cb013` | office_stringsearch2 | 2.41 | 1.1084 | **1.2445** | [1.105, 1.280] | ✓ |  | 1/2/0 | 18 | 4 | kernel_office_stringsearch2 (114) | dgx-spark-a-0 | 17s |
| `po_cb014` | security_rijndael_decode | 2.53 | 1.0000 | **1.0131** | [0.773, 1.548] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_security_rijndael_decode (60) | dgx-spark-a-0 | 20s |
| `po_cb015` | security_rijndael_encode | 2.90 | 1.2843 | **0.9490** | [0.703, 1.298] |  |  | 1/2/0 | 18 | 5 | kernel_security_rijndael_encode (60) | dgx-spark-a-0 | 21s |
| `po_cb016` | security_sha | 2.15 | 1.0000 | **1.0000** | — |  | ✓ | 0/0/3 | 1 | 0 | sha_transform (111) | dgx-spark-a-0 | 17s |
| `po_cb017` | telecom_adpcm_c | 4.93 | 2.1826 | **1.5151** | [1.109, 2.169] | ✓ |  | 2/1/0 | 22 | 7 | adpcm_coder (33) | dgx-spark-a-0 | 18s |
| `po_cb018` | telecom_adpcm_d | 2.97 | 1.3884 | **1.6012** | [1.504, 1.835] | ✓ |  | 2/1/0 | 17 | 4 | kernel_telecom_adpcm_d (30) | dgx-spark-a-0 | 17s |
| `po_cb019` | telecom_crc32 | 2.59 | 1.5646 | **0.7699** | [0.639, 0.858] |  |  | 1/2/0 | 16 | 13 | crc32file (22) | dgx-spark-a-0 | 19s |
| `po_pb001` | correlation | 9979.15 | 1.0289 | **1.0293** | [0.799, 1.515] | ✓ |  | 3/0/0 | 24 | 6 | kernel_correlation (47) | dgx-spark-a-0 | 14 |
| `po_pb002` | covariance | 10061.32 | 1.2940 | **1.0075** | [1.001, 1.008] | ✓ |  | 1/2/0 | 15 | 5 | kernel_covariance (31) | dgx-spark-b-0 | 13 |
| `po_pb003` | gemm | 609.00 | 1.0000 | **0.9940** | [0.994, 1.005] |  | ✓ | 0/2/1 | 1 | 0 | kernel_gemm (19) | dgx-spark-b-0 | 1 |
| `po_pb004` | gemver | 57.33 | 1.0000 | **1.0114** | [0.968, 1.015] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_gemver (32) | dgx-spark-b-0 | 21s |
| `po_pb005` | gesummv | 81.56 | 1.0000 | **0.9960** | [0.795, 1.059] |  | ✓ | 0/3/0 | 1 | 0 | kernel_gesummv (17) | dgx-spark-a-0 | 25s |
| `po_pb006` | symm | 5938.35 | 1.1822 | **0.9357** | [0.920, 1.056] |  |  | 1/2/0 | 18 | 6 | kernel_symm (16) | dgx-spark-b-0 | 10 |
| `po_pb007` | syr2k | 12296.21 | 1.2015 | **0.9890** | [0.979, 0.989] |  |  | 1/2/0 | 17 | 6 | kernel_syr2k (19) | dgx-spark-a-0 | 10 |
| `po_pb008` | syrk | 3164.56 | 1.0548 | **0.9822** | [0.816, 1.197] |  |  | 2/1/0 | 20 | 10 | kernel_syrk (18) | dgx-spark-b-0 | 5 |
| `po_pb009` | trmm | 4504.35 | 1.0076 | **1.0100** | [1.004, 1.011] | ✓ |  | 2/1/0 | 13 | 4 | kernel_trmm (14) | dgx-spark-a-0 | 6 |
| `po_pb010` | 2mm | 14557.90 | 1.1584 | **0.9958** | [0.915, 1.024] |  |  | 1/2/0 | 16 | 5 | kernel_2mm (28) | dgx-spark-b-0 | 20 |
| `po_pb011` | 3mm | 7523.16 | 1.0391 | **1.0366** | [0.986, 1.129] | ✓ |  | 1/2/0 | 18 | 7 | kernel_3mm (42) | dgx-spark-a-0 | 11 |
| `po_pb012` | atax | 51.55 | 1.0000 | **0.9854** | [0.883, 1.005] |  | ✓ | 0/3/0 | 1 | 0 | kernel_atax (20) | dgx-spark-a-0 | 20s |
| `po_pb013` | bicg | 76.43 | 1.1515 | **1.1853** | [1.130, 1.224] | ✓ |  | 1/2/0 | 15 | 6 | kernel_bicg (19) | dgx-spark-a-0 | 22s |
| `po_pb014` | doitgen | 1129.51 | 1.0320 | **1.0277** | [1.026, 1.029] | ✓ |  | 3/0/0 | 15 | 6 | kernel_doitgen (23) | dgx-spark-a-0 | 2 |
| `po_pb015` | mvt | 81.50 | 1.0000 | **1.0434** | [0.974, 1.044] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_mvt (19) | dgx-spark-a-0 | 21s |
| `po_pb016` | cholesky | 108475.84 | 2.2693 | **1.0060** | [0.802, 3.108] | ✓ |  | 1/2/0 | 17 | 6 | kernel_cholesky (20) | dgx-spark-b-0 | 88 |
| `po_pb017` | durbin | 4.19 | 1.0000 | **0.9950** | [0.995, 0.997] |  | ✓ | 0/3/0 | 1 | 0 | kernel_durbin (22) | dgx-spark-b-0 | 18s |
| `po_pb018` | gramschmidt | 7376.65 | 1.0111 | **1.0621** | [1.047, 1.163] | ✓ |  | 1/2/0 | 14 | 6 | kernel_gramschmidt (28) | dgx-spark-b-0 | 17 |
| `po_pb019` | ludcmp | 42183.19 | 1.0182 | **1.0009** | [0.761, 1.011] | ✓ |  | 1/2/0 | 34 | 6 | kernel_ludcmp (40) | dgx-spark-b-0 | 68 |
| `po_pb020` | lu | 55013.56 | 1.0444 | **1.0403** | [1.020, 1.050] | ✓ |  | 1/2/0 | 17 | 4 | kernel_lu (23) | dgx-spark-b-0 | 90 |
| `po_pb021` | trisolv | 26.58 | 1.0538 | **0.9559** | [0.679, 1.040] |  |  | 1/2/0 | 19 | 4 | kernel_trisolv (12) | dgx-spark-b-0 | 23s |
| `po_pb022` | deriche | 717.90 | 1.0093 | **1.0245** | [1.008, 1.025] | ✓ |  | 1/2/0 | 20 | 8 | kernel_deriche (84) | dgx-spark-b-0 | 2 |
| `po_pb023` | floyd-warshall | 47830.89 | 1.1594 | **1.1594** | [1.159, 1.159] | ✓ |  | 1/0/2 | 14 | 5 | kernel_floyd_warshall (13) | dgx-spark-b-0 | 38 |
| `po_pb024` | nussinov | 8274.22 | 1.0000 | **0.9993** | [0.998, 1.000] |  | ✓ | 0/3/0 | 1 | 0 | kernel_nussinov (32) | dgx-spark-b-0 | 9 |
| `po_pb025` | adi | 15658.84 | 1.0000 | **0.9997** | [1.000, 1.000] |  | ✓ | 0/3/0 | 1 | 0 | kernel_adi (53) | dgx-spark-b-0 | 31 |
| `po_pb026` | fdtd-2d | 1890.99 | 1.0000 | **1.0010** | [1.001, 1.004] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_fdtd_2d (34) | dgx-spark-b-0 | 4 |
| `po_pb027` | heat-3d | 4734.52 | 1.0000 | **1.0019** | [0.997, 1.002] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_heat_3d (44) | dgx-spark-b-0 | 12 |
| `po_pb028` | jacobi-1d | 5.68 | 1.0000 | **0.6929** | [0.591, 1.369] |  | ✓ | 0/3/0 | 1 | 0 | kernel_jacobi_1d (14) | dgx-spark-b-0 | 19s |
| `po_pb029` | jacobi-2d | 4618.36 | 1.0000 | **1.0002** | [1.000, 1.004] | ✓ | ✓ | 0/3/0 | 1 | 0 | kernel_jacobi_2d (22) | dgx-spark-b-0 | 6 |
| `po_pb030` | seidel-2d | 37870.23 | 1.0000 | **1.0000** | [1.000, 1.000] |  | ✓ | 0/3/0 | 1 | 0 | kernel_seidel_2d (13) | dgx-spark-a-0 | 51 |

### 4.1 PO 统计

| 指标 | 全部 | PolyBench | cBench |
|---|---:|---:|---:|
| n | 49 | 30 | 19 |
| **确认 geomean** | **1.0313** | **1.0024** | **1.0785** |
| 中位数 | 1.0010 | 1.0009 | 1.0017 |
| 胜 (>1.0) | 27 | 17 | 10 |
| 负 (<1.0) | 21 | 13 | 8 |
| 回退 default\<O3\> | 19 | 13 | 6 |
| significant | 27 | 17 | 10 |

### 4.2 搜索预算去向

- 候选 pipeline 总评估次数：**49×3 = 147**
- ACCEPTED（跑赢当前 P\*，成为新 P\*）：**44**
- REJECTED（没跑赢，回退 P\*）：**84**
- FAILED（`opt` 编译失败，整轮作废）：**19**（12.9% 的预算）
- 至少接受过一轮的程序：**30/49**

三轮全部编译失败（等于完全没被搜索，却计入 geomean 分母）的程序：

| 任务 | 程序 | FAILED 次数 | 确认值 |
|---|---|---:|---:|
| `po_cb004` | automotive_susan_smoothing | 3 | 1.0306 |
| `po_cb007` | consumer_tiff2bw | 3 | 1.4128 |
| `po_cb011` | network_dijkstra | 3 | 0.6653 |
| `po_cb016` | security_sha | 3 | 1.0000 |

### 4.3 与论文 Table 5 对齐

论文平台：LLVM 17.0.6；x86-64 (i9-11900K) 与 ARM64 (树莓派 5, Cortex-A76)。本项目为 ARM64 (Cortex-X925) + LLVM 21.1.8，应对比论文 ARM64 列。

| Suite | 本项目（确认口径） | 本项目（论文口径*） | 论文 x86-64 | 论文 ARM64 |
|---|---:|---:|---:|---:|
| cBench | 1.0785 (n=19) | 1.2140 | 1.059 | 1.111 |
| PolyBench | 1.0024 (n=30) | 1.0746 | 1.009 | 1.149 |
| **合计** | **1.0313** (n=49) | **1.1266** | **1.043** | **1.117** |

\* **论文口径 = 三轮搜索中观测到的最好值，不做独立复测。** 依据是 Table 5 表注原文 *"R3 denotes the **best performance in three refinement rounds**"*，以及附录中的 `AutoPass (best in R3) 1.040±0.114` 写法。本项目的"确认值"是把最终 pipeline 重新编译、与 -O3 交替配对复跑 n=3 后的结果。

**同口径下 1.127x vs 论文 ARM64 1.117x，基本重合。**两个口径之间 9.2% 的差额，即"3 次带噪测量取最大值"引入的 selection bias。
## 5. 条件 ①②③④（逐任务）

`探索期 best` = 9 步搜索过程中观测到的最好单次；`确认值` = 最终 pipeline 独立复测。`回退` 列记录最终确认阶段是否丢弃了探索期的 flags / 源码改动。

### 5.1 ① rewrite-only（n=50）

每步强制 `rewrite_source`，屏蔽编译器反馈。

| 任务 | 程序 | baseline (ms) | 步数 | 探索期 best | **确认值** | status | 显著 | 源码重写 | #flags | 回退 | 节点 | 用时(min) |
|---|---|---:|---:|---:|---:|---|:--:|:--:|---:|---|---|---:|
| `c1_cb001` | automotive_qsort1 | 8.26 | 9 | 1.2343 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-1 | 34 |
| `c1_cb002` | automotive_susan_corners | 4.80 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 62 |
| `c1_cb003` | automotive_susan_edges | 6.46 | 9 | 1.3817 | **1.0461** | confirmed |  | ✓ | 0 |  | dgx-spark-b-1 | 45 |
| `c1_cb004` | automotive_susan_smoothing | 61.84 | 9 | 1.5275 | **1.5422** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 47 |
| `c1_cb005` | bzip2_decode | 28.96 | 9 | 24.9198 | **98.8539** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 42 |
| `c1_cb006` | bzip2_encode | 49.58 | 9 | 1.1174 | **1.0161** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 32 |
| `c1_cb007` | consumer_tiff2bw | 1.92 | 9 | 1.1301 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 34 |
| `c1_cb008` | consumer_tiff2dither | 1.43 | 9 | 1.4579 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-1 | 37 |
| `c1_cb009` | consumer_tiff2median | 0.89 | 9 | 2.2749 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 15 |
| `c1_cb010` | consumer_tiff2rgba | 3.01 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 26s |
| `c1_cb011` | network_dijkstra | 1.10 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 9s |
| `c1_cb012` | network_patricia | 0.82 | 9 | 1.1860 | **1.2080** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 28 |
| `c1_cb013` | office_stringsearch2 | 1.30 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 9s |
| `c1_cb014` | security_rijndael_decode | 1.22 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 10s |
| `c1_cb015` | security_rijndael_encode | 1.27 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 10s |
| `c1_cb016` | security_sha | 2.10 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 9s |
| `c1_cb017` | telecom_adpcm_c | 1.57 | 9 | 1.0857 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 33 |
| `c1_cb018` | telecom_adpcm_d | 1.04 | 9 | 1.1450 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-1 | 28 |
| `c1_cb019` | telecom_crc32 | 0.83 | 9 | 1.0496 | **0.9498** | confirmed |  | ✓ | 0 |  | dgx-spark-b-2 | 30 |
| `c1_cb020` | security_blowfish_encode | 2.99 | 9 | 2.0526 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 56 |
| `c1_pb001` | correlation | 1451.01 | 9 | 11.5845 | **11.2504** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 37 |
| `c1_pb002` | covariance | 1447.97 | 9 | 14.7627 | **14.3160** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 52 |
| `c1_pb003` | gemm | 172.71 | 9 | 1.2907 | **1.2954** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 38 |
| `c1_pb004` | gemver | 18.46 | 9 | 2.0599 | **1.5884** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 36 |
| `c1_pb005` | gesummv | 20.97 | 9 | 1.3966 | **1.3846** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 31 |
| `c1_pb006` | symm | 951.00 | 9 | 5.5009 | **4.8076** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 51 |
| `c1_pb007` | syr2k | 1119.17 | 9 | 4.0431 | **4.0291** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 44 |
| `c1_pb008` | syrk | 354.80 | 9 | 2.7897 | **2.5274** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 36 |
| `c1_pb009` | trmm | 525.61 | 9 | 8.2966 | **6.6802** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 35 |
| `c1_pb010` | 2mm | 1082.42 | 9 | 7.2387 | **7.0366** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 50 |
| `c1_pb011` | 3mm | 1601.16 | 9 | 5.9006 | **5.8773** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 36 |
| `c1_pb012` | atax | 13.87 | 9 | 1.2443 | **1.0689** | confirmed |  | ✓ | 0 |  | dgx-spark-a-2 | 37 |
| `c1_pb013` | bicg | 23.77 | 9 | 1.6545 | **2.0693** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 46 |
| `c1_pb014` | doitgen | 242.92 | 9 | 3.9394 | **4.0545** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 40 |
| `c1_pb015` | mvt | 24.73 | 9 | 1.6088 | **1.2418** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 38 |
| `c1_pb016` | cholesky | 5778.15 | 9 | 1.1521 | **1.0960** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 72 |
| `c1_pb017` | durbin | 2.29 | 9 | 1.5346 | **1.8647** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 37 |
| `c1_pb018` | gramschmidt | 1519.15 | 9 | 6.9520 | **5.5517** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 39 |
| `c1_pb019` | ludcmp | 7042.83 | 9 | 1.2535 | **1.3086** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 63 |
| `c1_pb020` | lu | 7598.76 | 9 | 1.3246 | **1.1944** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 71 |
| `c1_pb021` | trisolv | 9.28 | 9 | 1.2759 | **1.0996** | confirmed |  | ✓ | 0 |  | dgx-spark-a-1 | 27 |
| `c1_pb022` | deriche | 133.68 | 9 | 1.9757 | **2.0006** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 39 |
| `c1_pb023` | floyd-warshall | 10335.66 | 9 | 7.0913 | **7.8767** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 79 |
| `c1_pb024` | nussinov | 1604.31 | 9 | 1.4263 | **1.3051** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-1 | 43 |
| `c1_pb025` | adi | 6598.28 | 9 | 4.1627 | **3.7707** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 68 |
| `c1_pb026` | fdtd-2d | 873.24 | 9 | 1.5688 | **1.2003** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 41 |
| `c1_pb027` | heat-3d | 916.78 | 9 | 1.4780 | **1.4455** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-2 | 42 |
| `c1_pb028` | jacobi-1d | 2.30 | 9 | 1.1950 | **1.0858** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 30 |
| `c1_pb029` | jacobi-2d | 599.09 | 9 | 1.2930 | **1.3387** | confirmed |  | ✓ | 0 |  | dgx-spark-a-2 | 44 |
| `c1_pb030` | seidel-2d | 13404.26 | 9 | 1.0120 | **1.0029** | confirmed |  | ✓ | 0 |  | dgx-spark-a-0 | 105 |

**小结**：geomean **1.8786**（PolyBench 2.4029 n=30；cBench 1.2986 n=20）／中位数 1.2249／区间 [0.9498, 98.8539]／探索期 geomean 2.0037／`baseline_only` 14 个／发生源码重写 36 个。

### 5.2 ② no-compiler-feedback（n=50）

自由选择动作，但屏蔽编译器反馈（不给 remark / IR / perf 证据）。

| 任务 | 程序 | baseline (ms) | 步数 | 探索期 best | **确认值** | status | 显著 | 源码重写 | #flags | 回退 | 节点 | 用时(min) |
|---|---|---:|---:|---:|---:|---|:--:|:--:|---:|---|---|---:|
| `c2_cb001` | automotive_qsort1 | 14.06 | 9 | 1.0137 | **1.0039** | confirmed |  |  | 2 |  | dgx-spark-a-0 | 19 |
| `c2_cb002` | automotive_susan_corners | 5.22 | 9 | 1.1452 | **1.0167** | confirmed | ✓ |  | 6 |  | dgx-spark-b-0 | 43 |
| `c2_cb003` | automotive_susan_edges | 8.12 | 9 | 1.0428 | **1.0265** | confirmed | ✓ |  | 4 |  | dgx-spark-a-0 | 27 |
| `c2_cb004` | automotive_susan_smoothing | 61.78 | 9 | 1.0018 | **1.0015** | confirmed |  | ✓ | 0 |  | dgx-spark-a-0 | 26 |
| `c2_cb005` | bzip2_decode | 48.64 | 9 | 1.0127 | **1.0211** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 19 |
| `c2_cb006` | bzip2_encode | 85.74 | 9 | 1.0030 | **0.9979** | confirmed |  | ✓ | 0 |  | dgx-spark-b-0 | 21 |
| `c2_cb007` | consumer_tiff2bw | 2.68 | 9 | 1.6964 | **1.0473** | confirmed |  |  | 4 |  | dgx-spark-b-0 | 20 |
| `c2_cb008` | consumer_tiff2dither | 2.36 | 6 | 1.1027 | **1.0119** | confirmed |  |  | 2 |  | dgx-spark-b-0 | 13 |
| `c2_cb009` | consumer_tiff2median | 1.98 | 9 | 2.0215 | **1.0319** | confirmed | ✓ |  | 2 |  | dgx-spark-b-0 | 5 |
| `c2_cb010` | consumer_tiff2rgba | 2.92 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 24s |
| `c2_cb011` | network_dijkstra | 0.82 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 13s |
| `c2_cb012` | network_patricia | 1.22 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 13s |
| `c2_cb013` | office_stringsearch2 | 1.12 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 13s |
| `c2_cb014` | security_rijndael_decode | 1.05 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 15s |
| `c2_cb015` | security_rijndael_encode | 2.15 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 15s |
| `c2_cb016` | security_sha | 0.88 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 13s |
| `c2_cb017` | telecom_adpcm_c | 2.37 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 13s |
| `c2_cb018` | telecom_adpcm_d | 1.28 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 14s |
| `c2_cb019` | telecom_crc32 | 0.91 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 13s |
| `c2_cb020` | security_blowfish_encode | 2.43 | 9 | 1.4541 | **1.0115** | confirmed |  | ✓ | 0 |  | dgx-spark-a-0 | 13 |
| `c2_pb001` | correlation | 1455.83 | 9 | 9.2431 | **9.2233** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-2 | 38 |
| `c2_pb002` | covariance | 1478.58 | 9 | 17.4164 | **15.0710** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-2 | 30 |
| `c2_pb003` | gemm | 279.95 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 30 |
| `c2_pb004` | gemver | 35.66 | 9 | 1.5126 | **1.5535** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 26 |
| `c2_pb005` | gesummv | 23.11 | 9 | 1.3175 | **1.3254** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-0 | 25 |
| `c2_pb006` | symm | 939.57 | 9 | 4.5795 | **4.3806** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-2 | 26 |
| `c2_pb007` | syr2k | 1168.11 | 9 | 3.1399 | **2.9951** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-0 | 27 |
| `c2_pb008` | syrk | 1021.38 | 9 | 1.7727 | **1.7276** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-0 | 25 |
| `c2_pb009` | trmm | 506.42 | 9 | 7.4840 | **7.4143** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-0 | 21 |
| `c2_pb010` | 2mm | 1072.18 | 9 | 5.6790 | **5.4802** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-1 | 23 |
| `c2_pb011` | 3mm | 1594.94 | 9 | 5.5404 | **4.9239** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-1 | 31 |
| `c2_pb012` | atax | 22.20 | 9 | 1.0673 | **1.0388** | confirmed |  | ✓ | 2 |  | dgx-spark-a-0 | 29 |
| `c2_pb013` | bicg | 27.06 | 9 | 2.2623 | **1.7205** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-0 | 24 |
| `c2_pb014` | doitgen | 246.75 | 9 | 4.1024 | **4.0125** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 24 |
| `c2_pb015` | mvt | 35.85 | 9 | 1.7018 | **1.6834** | confirmed | ✓ | ✓ | 4 |  | dgx-spark-b-0 | 23 |
| `c2_pb016` | cholesky | 27137.68 | 9 | 1.0248 | **1.0218** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 447 |
| `c2_pb017` | durbin | 3.32 | 9 | 1.6946 | **1.5706** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 24 |
| `c2_pb018` | gramschmidt | 1509.83 | 9 | 7.0014 | **5.9879** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-2 | 29 |
| `c2_pb019` | ludcmp | 7562.85 | 9 | 1.1682 | **1.0782** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-0 | 100 |
| `c2_pb020` | lu | 32724.41 | 9 | 1.2532 | **1.2560** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-0 | 319 |
| `c2_pb021` | trisolv | 10.85 | 9 | 1.1188 | **1.0927** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-0 | 23 |
| `c2_pb022` | deriche | 141.68 | 9 | 1.6412 | **1.5640** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-1 | 25 |
| `c2_pb023` | floyd-warshall | 10258.46 | 9 | 4.9622 | **3.7815** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-2 | 127 |
| `c2_pb024` | nussinov | 1385.63 | 9 | 1.3251 | **1.1087** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-b-1 | 45 |
| `c2_pb025` | adi | 6600.52 | 9 | 1.4993 | **1.5098** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-1 | 110 |
| `c2_pb026` | fdtd-2d | 750.00 | 9 | 1.5908 | **0.9989** | confirmed |  |  | 2 |  | dgx-spark-b-1 | 34 |
| `c2_pb027` | heat-3d | 2311.15 | 9 | 1.0734 | **1.0580** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-b-0 | 49 |
| `c2_pb028` | jacobi-1d | 2.53 | 9 | 1.6388 | **1.0432** | confirmed | ✓ |  | 2 |  | dgx-spark-b-0 | 19 |
| `c2_pb029` | jacobi-2d | 1123.69 | 9 | 1.1936 | **1.1948** | confirmed | ✓ |  | 4 |  | dgx-spark-a-0 | 40 |
| `c2_pb030` | seidel-2d | 18858.79 | 9 | 1.0167 | **1.0167** | confirmed | ✓ |  | 2 |  | dgx-spark-a-0 | 373 |

**小结**：geomean **1.5519**（PolyBench 2.0686 n=30；cBench 1.0084 n=20）／中位数 1.0453／区间 [0.9979, 15.0710]／探索期 geomean 1.6890／`baseline_only` 11 个／发生源码重写 29 个。

### 5.3 ③ full system（n=5）

自由选择动作 + 完整编译器反馈。**注意本条件在队列中只入了 6 个任务，样本量与其余条件不可比。**

| 任务 | 程序 | baseline (ms) | 步数 | 探索期 best | **确认值** | status | 显著 | 源码重写 | #flags | 回退 | 节点 | 用时(min) |
|---|---|---:|---:|---:|---:|---|:--:|:--:|---:|---|---|---:|
| `c3_cb012` | network_patricia | 0.87 | 9 | 1.0924 | **1.2095** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 24 |
| `c3_cb016` | security_sha | 0.86 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 75 |
| `c3_cb020` | security_blowfish_encode | 3.09 | 9 | 2.1138 | **1.4285** | confirmed | ✓ | ✓ | 2 |  | dgx-spark-a-0 | 20 |
| `c3_pb001` | correlation | 1456.78 | 9 | 13.9867 | **13.7896** | confirmed | ✓ | ✓ | 4 |  | dgx-spark-a-0 | 33 |
| `c3_pb021` | trisolv | 11.72 | 9 | 1.0668 | **1.0812** | confirmed | ✓ | ✓ | 0 |  | dgx-spark-a-0 | 35 |

**小结**：geomean **1.9151**（PolyBench 3.8612 n=2；cBench 1.1999 n=3）／中位数 1.2095／区间 [1.0000, 13.7896]／探索期 geomean 2.0298／`baseline_only` 1 个／发生源码重写 4 个。

### 5.4 ④ params-only（n=48）

每步强制 `try_flags`（只调编译选项，不改源码）。

| 任务 | 程序 | baseline (ms) | 步数 | 探索期 best | **确认值** | status | 显著 | 源码重写 | #flags | 回退 | 节点 | 用时(min) |
|---|---|---:|---:|---:|---:|---|:--:|:--:|---:|---|---|---:|
| `c4_cb001` | automotive_qsort1 | 8.95 | 9 | 1.0656 | **1.0040** | confirmed |  |  | 6 |  | dgx-spark-a-1 | 23 |
| `c4_cb002` | automotive_susan_corners | 3.04 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-1 | 20 |
| `c4_cb003` | automotive_susan_edges | 4.88 | 9 | 1.1466 | **1.0269** | confirmed |  |  | 2 |  | dgx-spark-a-1 | 34 |
| `c4_cb004` | automotive_susan_smoothing | 27.80 | 9 | 1.0007 | **1.0029** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 28 |
| `c4_cb005` | bzip2_decode | 29.73 | 9 | 1.0082 | **1.0128** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 59 |
| `c4_cb006` | bzip2_encode | 49.80 | 9 | 1.0133 | **1.0100** | confirmed |  |  | 2 |  | dgx-spark-a-1 | 41 |
| `c4_cb007` | consumer_tiff2bw | 1.31 | 9 | 1.3769 | **1.1575** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 38 |
| `c4_cb008` | consumer_tiff2dither | 1.50 | 9 | 1.1875 | **1.0146** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 57 |
| `c4_cb009` | consumer_tiff2median | 0.83 | 9 | 1.7507 | **1.0379** | confirmed | ✓ |  | 6 |  | dgx-spark-b-1 | 38 |
| `c4_cb010` | consumer_tiff2rgba | 1.92 | 9 | 1.1255 | **1.0304** | confirmed | ✓ |  | 6 |  | dgx-spark-b-1 | 58 |
| `c4_cb011` | network_dijkstra | 0.64 | 9 | 1.4169 | **1.0062** | confirmed | ✓ |  | 6 |  | dgx-spark-a-1 | 28 |
| `c4_cb012` | network_patricia | 0.55 | 9 | 1.1503 | **1.0171** | confirmed |  |  | 4 |  | dgx-spark-a-1 | 32 |
| `c4_cb013` | office_stringsearch2 | 0.89 | 9 | 1.6505 | **1.0698** | confirmed |  |  | 6 |  | dgx-spark-b-1 | 31 |
| `c4_cb014` | security_rijndael_decode | 0.69 | 9 | 1.3241 | **1.4484** | confirmed |  |  | 6 |  | dgx-spark-a-1 | 37 |
| `c4_cb015` | security_rijndael_encode | 0.87 | 9 | 1.4731 | **1.2612** | confirmed | ✓ |  | 2 |  | dgx-spark-a-2 | 34 |
| `c4_cb016` | security_sha | 0.90 | 9 | 1.8540 | **1.4529** | confirmed | ✓ |  | 6 |  | dgx-spark-b-1 | 121 |
| `c4_cb017` | telecom_adpcm_c | 1.30 | 9 | 1.2155 | **1.0369** | confirmed | ✓ |  | 4 |  | dgx-spark-b-2 | 28 |
| `c4_cb018` | telecom_adpcm_d | 1.16 | 9 | 1.7122 | **1.0264** | confirmed |  |  | 4 |  | dgx-spark-a-1 | 30 |
| `c4_cb019` | telecom_crc32 | 0.78 | 9 | 1.4610 | **1.6346** | confirmed | ✓ |  | 2 |  | dgx-spark-a-2 | 37 |
| `c4_pb001` | correlation | 1479.63 | 9 | 1.0170 | **1.0180** | confirmed | ✓ |  | 4 |  | dgx-spark-a-1 | 81 |
| `c4_pb002` | covariance | 4806.28 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 194 |
| `c4_pb003` | gemm | 195.90 | 9 | 1.1098 | **1.0680** | confirmed | ✓ |  | 2 |  | dgx-spark-b-1 | 39 |
| `c4_pb004` | gemver | 21.43 | 9 | 1.0320 | **1.0373** | confirmed | ✓ |  | 2 |  | dgx-spark-b-2 | 27 |
| `c4_pb005` | gesummv | 23.29 | 9 | 1.0262 | **0.9963** | confirmed |  |  | 2 |  | dgx-spark-b-0 | 33 |
| `c4_pb006` | symm | 898.86 | 9 | 1.0189 | **1.0086** | confirmed | ✓ |  | 6 |  | dgx-spark-a-2 | 124 |
| `c4_pb007` | syr2k | 1126.16 | 9 | 1.0506 | **1.0438** | confirmed | ✓ |  | 2 |  | dgx-spark-b-2 | 97 |
| `c4_pb008` | syrk | 1160.18 | 9 | 1.1560 | **1.0812** | confirmed | ✓ |  | 2 |  | dgx-spark-b-0 | 127 |
| `c4_pb009` | trmm | 524.19 | 9 | 1.0069 | **1.0007** | confirmed |  |  | 2 |  | dgx-spark-b-1 | 74 |
| `c4_pb010` | 2mm | 1192.42 | 9 | 1.2756 | **1.2449** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 155 |
| `c4_pb011` | 3mm | 3484.70 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-0 | 236 |
| `c4_pb012` | atax | 15.42 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-2 | 27 |
| `c4_pb013` | bicg | 23.89 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-a-2 | 27 |
| `c4_pb014` | doitgen | 240.89 | 9 | 1.0460 | **1.0542** | confirmed | ✓ |  | 2 |  | dgx-spark-b-2 | 60 |
| `c4_pb015` | mvt | 20.73 | 9 | 1.0723 | **1.0663** | confirmed | ✓ |  | 2 |  | dgx-spark-a-2 | 32 |
| `c4_pb016` | cholesky | 55009.01 | 9 | 1.0522 | **1.0652** | confirmed | ✓ |  | 2 |  | dgx-spark-a-0 | 625 |
| `c4_pb017` | durbin | 2.18 | 9 | 1.0498 | **1.1381** | confirmed | ✓ |  | 2 |  | dgx-spark-a-2 | 33 |
| `c4_pb018` | gramschmidt | 7172.84 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-0 | 297 |
| `c4_pb019` | ludcmp | 6877.13 | 9 | 1.0000 | **1.0000** | baseline_only |  |  | 0 |  | dgx-spark-b-2 | 65 |
| `c4_pb020` | lu | 7651.82 | 9 | 1.1078 | **1.1039** | confirmed | ✓ |  | 2 |  | dgx-spark-b-1 | 678 |
| `c4_pb021` | trisolv | 9.30 | 9 | 1.1273 | **1.0272** | confirmed |  |  | 2 |  | dgx-spark-a-1 | 33 |
| `c4_pb022` | deriche | 132.08 | 9 | 1.0129 | **1.0201** | confirmed | ✓ |  | 2 |  | dgx-spark-a-1 | 36 |
| `c4_pb023` | floyd-warshall | 10217.98 | 9 | 1.9251 | **1.8909** | confirmed | ✓ |  | 4 |  | dgx-spark-a-2 | 756 |
| `c4_pb024` | nussinov | 1606.71 | 9 | 1.1649 | **1.1689** | confirmed | ✓ |  | 4 |  | dgx-spark-a-1 | 162 |
| `c4_pb025` | adi | 6587.48 | 9 | 1.0856 | **1.0716** | confirmed | ✓ |  | 6 |  | dgx-spark-b-2 | 701 |
| `c4_pb026` | fdtd-2d | 975.32 | 9 | 1.0147 | **1.0208** | confirmed |  |  | 2 |  | dgx-spark-b-0 | 82 |
| `c4_pb027` | heat-3d | 1452.27 | 9 | 1.0660 | **1.0700** | confirmed | ✓ |  | 4 |  | dgx-spark-a-1 | 160 |
| `c4_pb028` | jacobi-1d | 1.59 | 9 | 1.0818 | **1.7305** | confirmed | ✓ |  | 2 |  | dgx-spark-b-0 | 24 |
| `c4_pb029` | jacobi-2d | 1181.78 | 9 | 1.1811 | **1.1786** | confirmed | ✓ |  | 2 |  | dgx-spark-b-0 | 86 |

**小结**：geomean **1.0980**（PolyBench 1.0932 n=29；cBench 1.1054 n=19）／中位数 1.0337／区间 [0.9963, 1.8909]／探索期 geomean 1.1590／`baseline_only` 7 个／发生源码重写 0 个。

## 6. OC — OpenCode + DeepSeek 外部 CLI agent baseline（n=49）

外部 agent 走独立 harness（`scripts/opencode_harness/run_one.sh`），只通过 shell 与代码交互，无 COMET 的结构化反馈。

| 任务 | 程序 | baseline (ms) | 探索期 best | **确认值** | IQR | 显著 | 节点 | 用时(min) |
|---|---|---:|---:|---:|---|:--:|---|---:|
| `oc_cb001` | automotive_qsort1 | 8.90 | 1.0021 | **0.9972** | [0.969, 1.002] |  | dgx-spark-b-1 | 6s |
| `oc_cb002` | automotive_susan_corners | 4.81 | 1.0248 | **0.9878** | [0.965, 1.025] |  | dgx-spark-b-0 | 8s |
| `oc_cb003` | automotive_susan_edges | 5.03 | 1.0116 | **0.9977** | [0.996, 1.012] |  | dgx-spark-b-1 | 8s |
| `oc_cb004` | automotive_susan_smoothing | 62.30 | 1.0039 | **1.0008** | [0.998, 1.004] | ✓ | dgx-spark-a-0 | 33s |
| `oc_cb005` | bzip2_decode | 29.40 | 1.0009 | **0.9975** | [0.996, 1.001] |  | dgx-spark-b-1 | 17s |
| `oc_cb006` | bzip2_encode | 49.49 | 1.0080 | **1.0073** | [0.994, 1.008] | ✓ | dgx-spark-b-1 | 18s |
| `oc_cb007` | consumer_tiff2bw | 2.79 | 1.0677 | **1.0027** | [0.965, 1.068] | ✓ | dgx-spark-b-0 | 18s |
| `oc_cb008` | consumer_tiff2dither | 1.52 | 1.0149 | **1.0017** | [0.971, 1.015] | ✓ | dgx-spark-b-1 | 18s |
| `oc_cb009` | consumer_tiff2median | 2.08 | 1.0207 | **0.9985** | [0.927, 1.021] |  | dgx-spark-b-0 | 18s |
| `oc_cb010` | consumer_tiff2rgba | 3.01 | 1.0565 | **1.0459** | [0.992, 1.057] | ✓ | dgx-spark-a-0 | 38s |
| `oc_cb011` | network_dijkstra | 2.06 | 1.9646 | **1.0044** | [1.000, 1.965] | ✓ | dgx-spark-b-0 | 5s |
| `oc_cb012` | network_patricia | 0.56 | 1.0153 | **0.9533** | [0.913, 1.015] |  | dgx-spark-b-1 | 5s |
| `oc_cb013` | office_stringsearch2 | 1.66 | 1.0044 | **0.9981** | [0.994, 1.004] |  | dgx-spark-b-0 | 5s |
| `oc_cb014` | security_rijndael_decode | 2.01 | 1.6449 | **1.0128** | [0.985, 1.645] | ✓ | dgx-spark-b-0 | 6s |
| `oc_cb015` | security_rijndael_encode | 2.08 | 1.0559 | **1.0013** | [0.920, 1.056] | ✓ | dgx-spark-b-0 | 6s |
| `oc_cb016` | security_sha | 1.87 | — | **1.0000** | — |  | dgx-spark-b-0 | 5s |
| `oc_cb017` | telecom_adpcm_c | 1.08 | 1.0016 | **0.9915** | [0.979, 1.002] |  | dgx-spark-b-1 | 5s |
| `oc_cb018` | telecom_adpcm_d | 0.76 | 1.0203 | **0.9779** | [0.976, 1.020] |  | dgx-spark-b-1 | 5s |
| `oc_cb019` | telecom_crc32 | 0.60 | 1.0096 | **1.0025** | [0.927, 1.010] | ✓ | dgx-spark-b-1 | 4s |
| `oc_pb001` | correlation | 1416.28 | — | **1.0000** | — |  | dgx-spark-a-2 | 24 |
| `oc_pb002` | covariance | 4912.74 | — | **1.0000** | — |  | dgx-spark-b-0 | 5 |
| `oc_pb003` | gemm | 288.19 | — | **1.0000** | — |  | dgx-spark-b-0 | 22s |
| `oc_pb004` | gemver | 36.22 | — | **1.0000** | — |  | dgx-spark-b-0 | 7s |
| `oc_pb005` | gesummv | 23.58 | — | **1.0000** | — |  | dgx-spark-b-0 | 6s |
| `oc_pb006` | symm | 4053.44 | — | **1.0000** | — |  | dgx-spark-b-0 | 4 |
| `oc_pb007` | syr2k | 1197.51 | — | **1.0000** | — |  | dgx-spark-a-1 | 15 |
| `oc_pb008` | syrk | 393.71 | — | **1.0000** | — |  | dgx-spark-b-1 | 29s |
| `oc_pb009` | trmm | 542.05 | — | **1.0000** | — |  | dgx-spark-b-1 | 38s |
| `oc_pb010` | 2mm | 1261.77 | — | **1.0000** | — |  | dgx-spark-b-1 | 1 |
| `oc_pb011` | 3mm | 4760.78 | — | **1.0000** | — |  | dgx-spark-b-0 | 5 |
| `oc_pb012` | atax | 15.44 | 1.0042 | **0.9759** | [0.914, 1.004] |  | dgx-spark-b-1 | 6s |
| `oc_pb013` | bicg | 26.05 | 1.0340 | **0.9994** | [0.994, 1.034] |  | dgx-spark-b-1 | 7s |
| `oc_pb014` | doitgen | 241.39 | — | **1.0000** | — |  | dgx-spark-b-1 | 20s |
| `oc_pb015` | mvt | 18.48 | — | **1.0000** | — |  | dgx-spark-b-1 | 6s |
| `oc_pb016` | cholesky | 6409.99 | — | **1.0000** | — |  | dgx-spark-b-1 | 7 |
| `oc_pb017` | durbin | 2.12 | 1.0256 | **1.0177** | [0.943, 1.026] | ✓ | dgx-spark-b-2 | 5s |
| `oc_pb018` | gramschmidt | 1499.48 | — | **1.0000** | — |  | dgx-spark-b-2 | 2 |
| `oc_pb019` | ludcmp | 34166.58 | — | **1.0000** | — |  | dgx-spark-b-0 | 33 |
| `oc_pb020` | lu | 7230.80 | — | **1.0000** | — |  | dgx-spark-b-2 | 7 |
| `oc_pb021` | trisolv | 9.83 | 1.0990 | **1.0254** | [1.024, 1.099] | ✓ | dgx-spark-b-1 | 6s |
| `oc_pb022` | deriche | 134.73 | — | **1.0000** | — |  | dgx-spark-b-1 | 13s |
| `oc_pb023` | floyd-warshall | 10439.90 | — | **1.0000** | — |  | dgx-spark-b-1 | 11 |
| `oc_pb024` | nussinov | 1598.64 | — | **1.0000** | — |  | dgx-spark-a-1 | 24 |
| `oc_pb025` | adi | 6589.41 | — | **1.0000** | — |  | dgx-spark-b-2 | 7 |
| `oc_pb026` | fdtd-2d | 469.20 | — | **1.0000** | — |  | dgx-spark-a-2 | 21 |
| `oc_pb027` | heat-3d | 1441.34 | — | **1.0000** | — |  | dgx-spark-b-1 | 2 |
| `oc_pb028` | jacobi-1d | 1.15 | 1.1687 | **1.1640** | [1.153, 1.169] | ✓ | dgx-spark-b-2 | 4s |
| `oc_pb029` | jacobi-2d | 696.12 | — | **1.0000** | — |  | dgx-spark-b-2 | 46s |
| `oc_pb030` | seidel-2d | 13396.17 | — | **1.0000** | — |  | dgx-spark-b-2 | 14 |

**小结**：geomean **1.0030**（PolyBench 1.0057；cBench 0.9988）／中位数 1.0000／区间 [0.9533, 1.1640]／>1.05 仅 1 个。
## 7. 正确性验证结果

最终确认阶段会把优化产物的输出与 -O3 参考输出逐值比对（PolyBench/数值型：相对误差 `epsilon=1e-4`；结构型：输出条目数）。比对失败记 `status=incorrect`，**该任务的加速比被强制置为 1.0000**（优化不算数），再计入 geomean。

| 条件 | 总数 | `incorrect` | 占比 |
|---|---:|---:|---:|
| OC | 49 | **26** | 53% |
| PO | 49 | **1** | 2% |
| ①②③④ | 194 | 0 | 0% |

> ①②③④ 走 COMET 自己的双重正确性校验（每步都验，失败当场回退），所以最终产物里不会残留 `incorrect`；OC/PO 是外部/复现 harness，只在最终确认阶段验一次，因此能观察到失败率。两者口径不同，**不能直接说"COMET 正确率更高"**，但可以说 OC 在无结构化反馈约束下产出的改动有一半通不过最终校验。

### 7.1 OC — 26/49 产出不正确的代码

其中 PolyBench 30 个程序里有 25 个失败。误差量级分两类：

- **灾难性**（相对误差 >1e-1）：`oc_pb001` 9.5e-1、`oc_pb007` 8.7e-1、`oc_pb024` 8.3e-1、`oc_pb020` 1.8e-1、`oc_pb019` 1.2e-1 — 计算结果实质性错误
- **边缘性**（1e-4 ~ 1e-3，刚越过 epsilon）：`oc_pb004` 1.02e-4、`oc_pb025` 1.28e-4、`oc_pb008` 1.80e-4 — 更像浮点重结合导致的精度漂移，判定是否算"错"取决于 epsilon 的取法

完整清单：

| 任务 | 程序 | baseline (ms) | 失败原因 |
|---|---|---:|---|
| `oc_cb016` | security_sha | 1.87 | Size mismatch: reference=4, optimized=3. Candidate likely changed the output structure. |
| `oc_pb001` | correlation | 1416.28 | Numeric mismatch: max relative error 9.52e-01 at index 0 (ref=1.401349, opt=0.067537), epsilon=1.00e-04 |
| `oc_pb002` | covariance | 4912.74 | Numeric mismatch: max relative error 1.70e-02 at index 0 (ref=1.45005, opt=1.425352), epsilon=1.00e-04 |
| `oc_pb003` | gemm | 288.19 | Numeric mismatch: max relative error 2.69e-02 at index 0 (ref=0.155941, opt=0.129075), epsilon=1.00e-04 |
| `oc_pb004` | gemver | 36.22 | Numeric mismatch: max relative error 1.02e-04 at index 0 (ref=0.00615, opt=0.006048), epsilon=1.00e-04 |
| `oc_pb005` | gesummv | 23.58 | Numeric mismatch: max relative error 1.30e-03 at index 0 (ref=0.004752, opt=0.006055), epsilon=1.00e-04 |
| `oc_pb006` | symm | 4053.44 | Numeric mismatch: max relative error 3.26e-03 at index 0 (ref=0.887359, opt=0.890619), epsilon=1.00e-04 |
| `oc_pb007` | syr2k | 1197.51 | Numeric mismatch: max relative error 8.68e-01 at index 0 (ref=1.104316, opt=0.146073), epsilon=1.00e-04 |
| `oc_pb008` | syrk | 393.71 | Numeric mismatch: max relative error 1.80e-04 at index 0 (ref=0.336116, opt=0.335936), epsilon=1.00e-04 |
| `oc_pb009` | trmm | 542.05 | Numeric mismatch: max relative error 2.26e-03 at index 0 (ref=0.510298, opt=0.508041), epsilon=1.00e-04 |
| `oc_pb010` | 2mm | 1261.77 | Numeric mismatch: max relative error 1.04e-03 at index 0 (ref=0.976865, opt=0.975826), epsilon=1.00e-04 |
| `oc_pb011` | 3mm | 4760.78 | Numeric mismatch: max relative error 5.34e-04 at index 0 (ref=1.672245, opt=1.673138), epsilon=1.00e-04 |
| `oc_pb014` | doitgen | 241.39 | Numeric mismatch: max relative error 2.27e-04 at index 0 (ref=0.235157, opt=0.23493), epsilon=1.00e-04 |
| `oc_pb015` | mvt | 18.48 | Numeric mismatch: max relative error 1.34e-03 at index 0 (ref=0.013098, opt=0.014434), epsilon=1.00e-04 |
| `oc_pb016` | cholesky | 6409.99 | Numeric mismatch: max relative error 3.10e-02 at index 0 (ref=0.687373, opt=0.718374), epsilon=1.00e-04 |
| `oc_pb018` | gramschmidt | 1499.48 | Numeric mismatch: max relative error 6.76e-04 at index 0 (ref=1.488523, opt=1.48953), epsilon=1.00e-04 |
| `oc_pb019` | ludcmp | 34166.58 | Numeric mismatch: max relative error 1.15e-01 at index 0 (ref=1.547051, opt=1.748996), epsilon=1.00e-04 |
| `oc_pb020` | lu | 7230.80 | Numeric mismatch: max relative error 1.84e-01 at index 0 (ref=2.064516, opt=1.68419), epsilon=1.00e-04 |
| `oc_pb022` | deriche | 134.73 | Numeric mismatch: max relative error 4.99e-03 at index 0 (ref=0.110071, opt=0.115062), epsilon=1.00e-04 |
| `oc_pb023` | floyd-warshall | 10439.90 | Numeric mismatch: max relative error 2.20e-03 at index 0 (ref=10.308704, opt=10.331475), epsilon=1.00e-04 |
| `oc_pb024` | nussinov | 1598.64 | Numeric mismatch: max relative error 8.32e-01 at index 0 (ref=1.337818, opt=0.22461), epsilon=1.00e-04 |
| `oc_pb025` | adi | 6589.41 | Numeric mismatch: max relative error 1.28e-04 at index 0 (ref=6.5809, opt=6.581743), epsilon=1.00e-04 |
| `oc_pb026` | fdtd-2d | 469.20 | Numeric mismatch: max relative error 1.25e-02 at index 0 (ref=0.468507, opt=0.456044), epsilon=1.00e-04 |
| `oc_pb027` | heat-3d | 1441.34 | Numeric mismatch: max relative error 4.93e-04 at index 0 (ref=1.374743, opt=1.375421), epsilon=1.00e-04 |
| `oc_pb029` | jacobi-2d | 696.12 | Numeric mismatch: max relative error 7.35e-02 at index 0 (ref=0.599908, opt=0.673365), epsilon=1.00e-04 |
| `oc_pb030` | seidel-2d | 13396.17 | Numeric mismatch: max relative error 2.71e-04 at index 0 (ref=13.227689, opt=13.2241), epsilon=1.00e-04 |

### 7.2 PO — 1 个失败，且很可能是 shim 的问题

`po_cb016` (security_sha)：`Size mismatch: reference=4, optimized=3. Candidate likely changed the output structure.`

**这个失败值得单独怀疑 harness 而不是候选。** 两点理由：

1. PO 只重排 LLVM pass 顺序与调整 pass 参数，**不改源码**。合法的 pass 组合把输出条目数从 4 变成 3，要么是真实的 miscompilation（应当当作 LLVM bug 单独上报），要么是 shim 的输出逻辑本身不稳定。
2. **同一个程序在完全不同的 harness 下报出了字节级相同的错误**（`oc_cb016` 也是 `Size mismatch: reference=4, optimized=3`）。两套独立实现同时踩到同一个坑，指向 `security_sha` shim 的输出条目数不确定，而非两个 agent 各自巧合地犯了同样的错。

建议先单独复核 `CBench_shim_root/cbench-security-sha/` 的输出打印逻辑，再决定这两个数据点如何处理。
## 8. 数据质量问题清单

### 8.1 两个孤儿进程长期抢占测量核（已于 2026-07-31 处理）

两台节点上各有一个 `PPID=1` 的 `optimize.py --params-only` 遗留进程，均 `--pin-cpu 2`——正是 worker 用于计时的同一个核。父 worker 早已退出，它们的结果不会回写队列，纯粹占用测量核。

| 节点 | PID | 程序 | 起始（UTC） | 存活时长 | 处理 |
|---|---:|---|---|---:|---|
| dgx-spark-a | 1539565 | cholesky --params-only | 07-30 ~18:01 | 23h21m | 已 kill（含子进程 tf_46_64） |
| dgx-spark-b | 716932 | seidel-2d --params-only | 07-30 ~20:08 | 21h14m | 已 kill（含子进程 tf_verify_best） |

**影响范围：在该时间窗内完成的任务共 53 个**，其中：

| 条件 | 受影响任务数 |
|---|---:|
| c1 | 1 |
| c2 | 1 |
| c3 | 1 |
| c4 | 1 |
| po | 49 |

**PO 的 49 个任务全部落在该窗口内。** 这些结果的噪声底偏高，绝对值需谨慎引用；配对交替测量能吸收缓慢漂移，但吸收不了同核争抢带来的方差。

### 8.2 自校准控制组：回退到 `default<O3>` 的任务应当恰好 1.000

PO 中有 19 个任务三轮全败、最终回退到 LLVM 自带 `default<O3>` pipeline。这些任务的最终二进制在语义上就是 -O3 本身，确认值理论上应为 1.000。它们构成一个**免费的测量精度标尺**：

| 子集 | n | 平均绝对偏差 | 最小 | 最大 |
|---|---:|---:|---:|---:|
| PolyBench | 13 | 3.0% | 0.6929 | 1.0434 |
| cBench | 6 | 16.3% | 0.6653 | 1.4128 |

偏离 1.000 超过 5% 的控制组任务（这些数字是纯噪声，不含任何优化）：

| 任务 | 程序 | 确认值 | baseline (ms) |
|---|---|---:|---:|
| `po_cb007` | consumer_tiff2bw | **1.4128** | 5.07 |
| `po_cb011` | network_dijkstra | **0.6653** | 2.14 |
| `po_pb028` | jacobi-1d | **0.6929** | 5.68 |
| `po_cb010` | consumer_tiff2rgba | **1.1858** | 9.22 |

**结论：cBench 子集的测量精度约 ±16%，PolyBench 约 ±3%。**短基准（baseline < 100ms）上任何 10–20% 的"提升"都不可信。

### 8.3 探索期最好值与最终确认值严重不一致

`确认值 / 探索期 best` 落在 [0.7, 1.3] 之外的任务。两个方向都有问题：偏低说明探索期读数虚高，**偏高则完全没有物理解释**——最终 pipeline 不可能比搜索过程中测到的最好结果还快。

| 任务 | 程序 | 探索期 best | 确认值 | 比值 | baseline (ms) |
|---|---|---:|---:|---:|---:|
| `c1_cb005` | bzip2_decode | 24.920 | 98.854 | 3.97 | 28.96 |
| `po_pb016` | cholesky | 2.269 | 1.006 | 0.44 | 108475.84 |
| `po_cb019` | telecom_crc32 | 1.565 | 0.770 | 0.49 | 2.59 |
| `c2_cb009` | consumer_tiff2median | 2.022 | 1.032 | 0.51 | 1.98 |
| `oc_cb011` | network_dijkstra | 1.965 | 1.004 | 0.51 | 2.06 |
| `po_cb008` | consumer_tiff2dither | 1.704 | 0.970 | 0.57 | 7.99 |
| `po_cb009` | consumer_tiff2median | 1.743 | 1.000 | 0.57 | 2.15 |
| `c4_cb009` | consumer_tiff2median | 1.751 | 1.038 | 0.59 | 0.83 |
| `c4_cb018` | telecom_adpcm_d | 1.712 | 1.026 | 0.60 | 1.16 |
| `oc_cb014` | security_rijndael_decode | 1.645 | 1.013 | 0.62 | 2.01 |
| `c2_cb007` | consumer_tiff2bw | 1.696 | 1.047 | 0.62 | 2.68 |
| `c4_pb028` | jacobi-1d | 1.082 | 1.730 | 1.60 | 1.59 |
| `c2_pb026` | fdtd-2d | 1.591 | 0.999 | 0.63 | 750.00 |
| `c2_pb028` | jacobi-1d | 1.639 | 1.043 | 0.64 | 2.53 |
| `c4_cb013` | office_stringsearch2 | 1.651 | 1.070 | 0.65 | 0.89 |
| `po_cb011` | network_dijkstra | 1.000 | 0.665 | 0.67 | 2.14 |
| `c3_cb020` | security_blowfish_encode | 2.114 | 1.428 | 0.68 | 3.09 |
| `po_pb028` | jacobi-1d | 1.000 | 0.693 | 0.69 | 5.68 |
| `po_cb017` | telecom_adpcm_c | 2.183 | 1.515 | 0.69 | 4.93 |
| `c2_cb020` | security_blowfish_encode | 1.454 | 1.011 | 0.70 | 2.43 |
| `po_cb007` | consumer_tiff2bw | 1.000 | 1.413 | 1.41 | 5.07 |

共 21 个，其中 3 个是"确认值反而更高"。

### 8.4 `c1_cb005` (bzip2_decode) 报告 98.85x — 高度可疑

- 探索期最好单次：24.9198x
- 最终确认值：**98.8539x**（`status=confirmed`，`significant=True`）
- baseline：28.96 ms
- 日志中正确性验证模式为 **`numeric`**

三个独立的疑点：(1) bzip2 解压不存在 ~99 倍的优化空间；(2) 最终确认值比探索期最好值还高 4 倍，自相矛盾；(3) bzip2 解压的正确性必须比对输出字节流，`numeric` 模式（数值近似比对）对"解压逻辑被删掉"这类改动是拦不住的。

**它单独把条件 ① 的 geomean 从 1.7326 抬到 1.8786。**在采信前必须人工核对 `bzip2_decode_optimized.c` 的输出是否与原程序逐字节一致。

### 8.5 条件 ③ 样本量严重不足

③ full system 在队列中总共只入了 6 个任务，已完成 5 个，而 ①②④ 各约 50 个。当前 ③ 的 geomean 1.9151 由 5 个样本得出，且其中 `c3_pb001` (correlation) 一个就是 13.79x。**这个数字不能与其它条件并列比较。**若 ③ 是论文主结果，这是目前最大的缺口。

### 8.6 PO 的 13% 搜索预算浪费在编译失败上

147 次候选评估中有 19 次因 `opt` 拒绝 Reasoning Agent 提出的 pass 名 / 参数而整轮作废，difflib 确定性修复没有兜住。三轮全失败的程序等于完全没被搜索，却以 1.000x 计入 geomean 分母。修掉这部分后 PO 的结果才算干净。

## 9. 数据来源与复算方法

本文件所有数字的提取链路，可逐步复算：

1. **任务→节点归属**：`oracle4:/home/hanning/comet_queue/state.json`。同一个 task id 在不同节点上可能存在历史日志（重跑前的残留），**必须以 state 中记录的 `node` 为准**，否则会读到作废数据。
2. **条件 ①②③④**：读该节点 `~/comet/logs_queue_run_v2/<id>.log` 里 `结果 JSON:` 指向的路径，取 `final_speedup` / `final_status` / `best_speedup` / `significant_gain` / `has_source_rewrite` / `rolled_back_flags` / `rolled_back_source` / `steps_taken`。
3. **OC**：日志末尾的 JSON 块，取 `confirmed_speedup` / `best_speedup` / `speedup_iqr`。
4. **PO**：日志中的 JSON 块，取 `confirmed_speedup` / `explored_best_speedup` / `no_improvement_over_O3` / `speedup_iqr` / `best_passes` / `best_params` / `score_agent_target`；轮次统计由日志正文中 `ACCEPTED` / `REJECTED` / `FAILED` 的出现次数计数。
5. **新旧 harness 判别**：PO 日志中是否存在 `no_improvement_over_O3` 字段。该字段是三个测量 bug 修复时引入的，缺失即为作废数据。本次核验 49/49 个 PO 任务全部带该字段。
6. **汇总口径**：geomean 为所有任务确认值的几何平均，不做任何剔除（包括 §8.4 的 98.85x 异常值——它在本文件中原样保留）。

## 10. 待办

- [ ] 核对 `c1_cb005` (bzip2_decode) 98.85x 的正确性，并把 cBench 的正确性验证从 `numeric` 换成字节流比对
- [ ] 修 Reasoning Agent 的 pass/参数校验，消掉 PO 的 19 次编译失败后重跑受影响程序
- [ ] 补齐条件 ③ 的样本量（当前仅 5 个 vs 其余约 50 个）
- [ ] 孤儿进程已清除，考虑重跑受污染窗口内的 cBench PO 任务（控制组显示该子集精度仅 ±16%）
- [ ] 收尾剩余 10 个任务（blowfish encode/decode 两个程序 × 6 条件）
- [ ] 复核 `security_sha` shim 的输出条目数（OC 与 PO 两套 harness 报出同一个 `Size mismatch`，指向 shim 而非候选）
- [ ] 决定 OC 的 26 个 `incorrect` 在论文中如何呈现：是并入 geomean 记 1.0，还是单列"正确率"指标
- [ ] 论文中同时报告两个口径的 PO 数字，并说明差异来源
