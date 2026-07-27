# 151任务消融实验完整结果 (2026-07-27)

覆盖49个程序（30 PolyBench + 19 cBench），全部在两台DGX机器上运行（不含oracle4），每个comet条件`--rounds 9 --runs 3`，测量统一采用**4次取样去掉最高最低取中间2次均值**的trimmed-mean方式，并发worker之间通过`taskset --pin-cpu`隔离到独立物理核心，避免跨进程调度噪声。全部151个任务0进程崩溃/超时。

## ⚠️ 关键发现：OpenCode+DeepSeek baseline 一半以上产出"正确性错误"的代码

OpenCode（通用coding agent，无compiler feedback，只能靠自己跑`./measure.sh`摸索）的49个任务里，**26个（53%）最终代码没有通过正确性校验**（数值结果与-O3基线不一致），被记为`incorrect`；只有23个（47%）产出了正确的代码，其中大部分（\~19个）仍然拿不到有意义的加速。

这是一个比"没有加速"更严重的问题：说明通用coding agent在没有细粒度反馈的情况下，不仅优化能力弱，**连基本的正确性都保不住**——这恰好印证了「只靠agent自己盲测，没有编译器/正确性层面的结构化反馈是不可靠的」这一论点，可以作为baseline对比里很有分量的一条。

## 1. 总体统计（区分 correct-但无提升 / incorrect / 有效加速）

| 条件 | n | correct且有加速(>1.0x) | correct但≤1.0x | incorrect(正确性失败) | baseline_only(未找到候选) |
|---|---|---|---|---|---|
| ① 只改代码（无编译反馈） | 49 | 40 | 1 | 0 | 8 |
| ② 改代码+调参（无编译反馈） | 49 | 42 | 5 | 0 | 2 |
| ③ 完整系统（代码+调参+编译反馈） | 4 | 2 | 1 | 0 | 1 |
| OC OpenCode+DeepSeek baseline | 49 | 11 | 12 | 26 | 0 |

## 2. 加速比统计（仅对正确的结果计算，排除incorrect的失真1.0x）

| 条件 | n(有效) | 几何均值 | 中位数 | 均值 | 范围 |
|---|---|---|---|---|---|
| ① 只改代码（无编译反馈） | 49 | 1.919x | 1.242x | 4.498x | 0.950x – 98.854x |
| ② 改代码+调参（无编译反馈） | 49 | 1.570x | 1.109x | 2.201x | 0.992x – 15.071x |
| ③ 完整系统（代码+调参+编译反馈） | 4 | 2.067x | 1.163x | 4.278x | 0.999x – 13.790x |
| OC OpenCode+DeepSeek baseline | 23 | 1.006x | 0.999x | 1.006x | 0.953x – 1.164x |

> **注**: ③（完整系统）目前只有4个样本，是之前误跑在oracle4上的4个任务（correlation、trisolv、network_patricia、security_sha）
> 在dgx上的重跑；其余45个完整系统结果复用此前独立跑完的49任务全量运行（未采用本次trimmed-mean-of-4测量法，
> 口径不完全一致，尚未纳入本文档的统一对比，需要后续单独重跑或统一转换后再合并）。

## 3. 按加速比区间分布（仅correct结果）

| 区间 | ① | ② | ③ | OC |
|---|---|---|---|---|
| <=1.0x (correct但无提升) | 9 | 7 | 2 | 12 |
| 1.0-1.2x | 13 | 24 | 0 | 11 |
| 1.2-1.5x | 9 | 5 | 1 | 0 |
| 1.5-2x | 3 | 3 | 0 | 0 |
| 2-5x | 7 | 5 | 0 | 0 |
| >5x | 8 | 5 | 1 | 0 |
| incorrect(正确性失败) | 0 | 0 | 0 | 26 |

## 4. 各条件详细任务表


### ① 只改代码（无编译反馈） (n=49)

| 程序 | 数据集 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|---|
| bzip2_decode | cbench | 28.960 | 24.920x | **98.854x** | confirmed | True | 3 | dgx-spark-a-2 |
| covariance | polybench | 1447.970 | 14.763x | **14.316x** | confirmed | True | 3 | dgx-spark-a-1 |
| correlation | polybench | 1451.010 | 11.585x | **11.250x** | confirmed | True | 3 | dgx-spark-b-1 |
| floyd-warshall | polybench | 10335.660 | 7.091x | **7.877x** | confirmed | True | 3 | dgx-spark-a-2 |
| 2mm | polybench | 1082.420 | 7.239x | **7.037x** | confirmed | True | 3 | dgx-spark-b-2 |
| trmm | polybench | 525.610 | 8.297x | **6.680x** | confirmed | True | 3 | dgx-spark-a-2 |
| 3mm | polybench | 1601.160 | 5.901x | **5.877x** | confirmed | True | 3 | dgx-spark-a-1 |
| gramschmidt | polybench | 1519.150 | 6.952x | **5.552x** | confirmed | True | 3 | dgx-spark-a-0 |
| symm | polybench | 951.000 | 5.501x | **4.808x** | confirmed | True | 3 | dgx-spark-a-1 |
| doitgen | polybench | 242.920 | 3.939x | **4.054x** | confirmed | True | 3 | dgx-spark-a-0 |
| syr2k | polybench | 1119.170 | 4.043x | **4.029x** | confirmed | True | 3 | dgx-spark-b-2 |
| adi | polybench | 6598.280 | 4.163x | **3.771x** | confirmed | True | 3 | dgx-spark-a-0 |
| syrk | polybench | 354.800 | 2.790x | **2.527x** | confirmed | True | 3 | dgx-spark-b-1 |
| bicg | polybench | 23.770 | 1.655x | **2.069x** | confirmed | True | 3 | dgx-spark-a-1 |
| deriche | polybench | 133.680 | 1.976x | **2.001x** | confirmed | True | 3 | dgx-spark-b-1 |
| durbin | polybench | 2.290 | 1.535x | **1.865x** | confirmed | True | 3 | dgx-spark-a-1 |
| gemver | polybench | 18.460 | 2.060x | **1.588x** | confirmed | True | 3 | dgx-spark-b-2 |
| automotive_susan_smoothing | cbench | 43.500 | 1.305x | **1.546x** | confirmed | False | 3 | dgx-spark-a-1 |
| heat-3d | polybench | 916.780 | 1.478x | **1.446x** | confirmed | True | 3 | dgx-spark-a-2 |
| gesummv | polybench | 20.970 | 1.397x | **1.385x** | confirmed | True | 3 | dgx-spark-b-1 |
| jacobi-2d | polybench | 599.090 | 1.293x | **1.339x** | confirmed | False | 3 | dgx-spark-a-2 |
| ludcmp | polybench | 7042.830 | 1.254x | **1.309x** | confirmed | True | 3 | dgx-spark-a-2 |
| nussinov | polybench | 1604.310 | 1.426x | **1.305x** | confirmed | True | 3 | dgx-spark-a-1 |
| gemm | polybench | 172.710 | 1.291x | **1.295x** | confirmed | True | 3 | dgx-spark-a-0 |
| mvt | polybench | 24.730 | 1.609x | **1.242x** | confirmed | True | 3 | dgx-spark-b-2 |
| network_patricia | cbench | 0.820 | 1.186x | **1.208x** | confirmed | True | 3 | dgx-spark-a-2 |
| fdtd-2d | polybench | 873.240 | 1.569x | **1.200x** | confirmed | True | 3 | dgx-spark-b-0 |
| lu | polybench | 7598.760 | 1.325x | **1.194x** | confirmed | True | 3 | dgx-spark-b-2 |
| automotive_susan_corners | cbench | 5.260 | 1.439x | **1.174x** | confirmed | False | 3 | dgx-spark-a-2 |
| consumer_tiff2bw | cbench | 1.740 | 1.481x | **1.155x** | confirmed | True | 3 | dgx-spark-a-2 |
| trisolv | polybench | 9.280 | 1.276x | **1.100x** | confirmed | False | 3 | dgx-spark-a-1 |
| cholesky | polybench | 5778.150 | 1.152x | **1.096x** | confirmed | True | 3 | dgx-spark-b-0 |
| jacobi-1d | polybench | 2.300 | 1.195x | **1.086x** | confirmed | True | 3 | dgx-spark-a-0 |
| atax | polybench | 13.870 | 1.244x | **1.069x** | confirmed | False | 3 | dgx-spark-a-2 |
| network_dijkstra | cbench | 0.970 | 1.138x | **1.062x** | confirmed | False | 3 | dgx-spark-a-1 |
| automotive_susan_edges | cbench | 6.460 | 1.382x | **1.046x** | confirmed | False | 3 | dgx-spark-b-1 |
| office_stringsearch2 | cbench | 0.590 | 1.070x | **1.037x** | confirmed | True | 3 | dgx-spark-b-1 |
| bzip2_encode | cbench | 49.580 | 1.117x | **1.016x** | confirmed | True | 3 | dgx-spark-b-0 |
| seidel-2d | polybench | 13404.260 | 1.012x | **1.003x** | confirmed | False | 3 | dgx-spark-a-0 |
| security_rijndael_decode | cbench | 0.980 | 1.047x | **1.002x** | confirmed | False | 3 | dgx-spark-b-0 |
| consumer_tiff2dither | cbench | 1.430 | 1.458x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-1 |
| security_rijndael_encode | cbench | 0.910 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-2 |
| telecom_adpcm_d | cbench | 1.040 | 1.145x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-1 |
| automotive_qsort1 | cbench | 8.260 | 1.234x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-1 |
| consumer_tiff2median | cbench | 0.690 | 2.752x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-1 |
| consumer_tiff2rgba | cbench | 1.470 | 1.548x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-0 |
| security_sha | cbench | 0.630 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-2 |
| telecom_adpcm_c | cbench | 1.570 | 1.086x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-0 |
| telecom_crc32 | cbench | 0.830 | 1.050x | **0.950x** | confirmed | False | 3 | dgx-spark-b-2 |

### ② 改代码+调参（无编译反馈） (n=49)

| 程序 | 数据集 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|---|
| covariance | polybench | 1478.580 | 17.416x | **15.071x** | confirmed | True | 3 | dgx-spark-a-2 |
| correlation | polybench | 1455.830 | 9.243x | **9.223x** | confirmed | True | 3 | dgx-spark-b-2 |
| trmm | polybench | 506.420 | 7.484x | **7.414x** | confirmed | True | 3 | dgx-spark-a-0 |
| gramschmidt | polybench | 1509.830 | 7.001x | **5.988x** | confirmed | True | 3 | dgx-spark-b-2 |
| 2mm | polybench | 1072.180 | 5.679x | **5.480x** | confirmed | True | 3 | dgx-spark-a-1 |
| 3mm | polybench | 1594.940 | 5.540x | **4.924x** | confirmed | True | 3 | dgx-spark-b-1 |
| symm | polybench | 939.570 | 4.580x | **4.381x** | confirmed | True | 3 | dgx-spark-a-2 |
| doitgen | polybench | 246.750 | 4.102x | **4.013x** | confirmed | True | 3 | dgx-spark-b-1 |
| floyd-warshall | polybench | 10258.460 | 4.962x | **3.781x** | confirmed | True | 3 | dgx-spark-b-2 |
| syr2k | polybench | 1168.110 | 3.140x | **2.995x** | confirmed | True | 3 | dgx-spark-a-0 |
| bicg | polybench | 27.060 | 2.262x | **1.720x** | confirmed | True | 3 | dgx-spark-b-0 |
| deriche | polybench | 141.680 | 1.641x | **1.564x** | confirmed | True | 3 | dgx-spark-a-1 |
| adi | polybench | 6600.520 | 1.499x | **1.510x** | confirmed | True | 3 | dgx-spark-a-1 |
| network_dijkstra | cbench | 0.730 | 2.579x | **1.454x** | confirmed | True | 3 | dgx-spark-a-0 |
| gesummv | polybench | 17.890 | 1.745x | **1.402x** | confirmed | True | 3 | dgx-spark-a-0 |
| automotive_susan_edges | cbench | 4.550 | 1.061x | **1.381x** | confirmed | True | 3 | dgx-spark-b-0 |
| automotive_qsort1 | cbench | 11.430 | 1.780x | **1.331x** | confirmed | True | 3 | dgx-spark-b-0 |
| automotive_susan_corners | cbench | 3.140 | 1.186x | **1.260x** | confirmed | True | 3 | dgx-spark-a-1 |
| telecom_adpcm_d | cbench | 1.020 | 1.355x | **1.191x** | confirmed | True | 3 | dgx-spark-a-2 |
| lu | polybench | 7508.260 | 1.163x | **1.191x** | confirmed | True | 3 | dgx-spark-b-0 |
| mvt | polybench | 20.440 | 1.237x | **1.182x** | confirmed | True | 3 | dgx-spark-a-2 |
| gemver | polybench | 21.820 | 1.311x | **1.174x** | confirmed | True | 3 | dgx-spark-a-2 |
| trisolv | polybench | 11.100 | 1.191x | **1.153x** | confirmed | True | 3 | dgx-spark-a-2 |
| telecom_adpcm_c | cbench | 1.130 | 1.118x | **1.151x** | confirmed | True | 3 | dgx-spark-b-1 |
| nussinov | polybench | 1385.630 | 1.325x | **1.109x** | confirmed | True | 3 | dgx-spark-b-1 |
| syrk | polybench | 330.300 | 1.060x | **1.100x** | confirmed | True | 3 | dgx-spark-b-0 |
| consumer_tiff2rgba | cbench | 1.850 | 1.404x | **1.097x** | confirmed | False | 3 | dgx-spark-a-2 |
| atax | polybench | 15.960 | 1.047x | **1.091x** | confirmed | True | 3 | dgx-spark-a-0 |
| cholesky | polybench | 6176.480 | 1.089x | **1.083x** | confirmed | True | 3 | dgx-spark-b-1 |
| ludcmp | polybench | 7562.850 | 1.168x | **1.078x** | confirmed | True | 3 | dgx-spark-a-0 |
| office_stringsearch2 | cbench | 0.870 | 1.098x | **1.066x** | confirmed | True | 3 | dgx-spark-b-2 |
| jacobi-1d | polybench | 0.750 | 1.412x | **1.058x** | confirmed | False | 3 | dgx-spark-b-0 |
| consumer_tiff2bw | cbench | 1.090 | 1.479x | **1.048x** | confirmed | True | 3 | dgx-spark-a-0 |
| telecom_crc32 | cbench | 0.490 | 1.171x | **1.044x** | confirmed | False | 3 | dgx-spark-b-1 |
| network_patricia | cbench | 0.950 | 1.397x | **1.038x** | confirmed | True | 3 | dgx-spark-b-0 |
| heat-3d | polybench | 1232.540 | 1.284x | **1.022x** | confirmed | True | 3 | dgx-spark-b-1 |
| durbin | polybench | 1.670 | 1.017x | **1.019x** | confirmed | True | 3 | dgx-spark-a-2 |
| seidel-2d | polybench | 18858.790 | 1.017x | **1.017x** | confirmed | True | 3 | dgx-spark-a-0 |
| security_rijndael_encode | cbench | 0.850 | 1.599x | **1.016x** | confirmed | False | 3 | dgx-spark-b-1 |
| gemm | polybench | 137.400 | 1.036x | **1.015x** | confirmed | True | 3 | dgx-spark-b-1 |
| security_rijndael_decode | cbench | 0.960 | 1.337x | **1.006x** | confirmed | False | 3 | dgx-spark-b-2 |
| automotive_susan_smoothing | cbench | 28.200 | 1.553x | **1.004x** | confirmed | False | 3 | dgx-spark-b-0 |
| security_sha | cbench | 0.950 | 1.138x | **1.000x** | baseline_only | False | 0 | dgx-spark-a-1 |
| jacobi-2d | polybench | 770.520 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-0 |
| fdtd-2d | polybench | 750.000 | 1.591x | **0.999x** | confirmed | False | 3 | dgx-spark-b-1 |
| bzip2_decode | cbench | 29.020 | 1.014x | **0.998x** | confirmed | False | 3 | dgx-spark-b-1 |
| bzip2_encode | cbench | 48.890 | 1.106x | **0.997x** | confirmed | False | 3 | dgx-spark-b-1 |
| consumer_tiff2dither | cbench | 2.150 | 1.604x | **0.996x** | confirmed | False | 3 | dgx-spark-b-0 |
| consumer_tiff2median | cbench | 0.400 | 2.429x | **0.992x** | confirmed | False | 3 | dgx-spark-a-0 |

### ③ 完整系统（代码+调参+编译反馈） (n=4)

| 程序 | 数据集 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|---|
| correlation | polybench | 1456.780 | 13.987x | **13.790x** | confirmed | True | 3 | dgx-spark-a-0 |
| network_patricia | cbench | 0.820 | 2.662x | **1.325x** | confirmed | False | 3 | dgx-spark-a-2 |
| security_sha | cbench | 0.560 | 1.000x | **1.000x** | baseline_only | False | 0 | dgx-spark-b-0 |
| trisolv | polybench | 9.900 | 1.274x | **0.999x** | confirmed | False | 3 | dgx-spark-a-1 |

### OC OpenCode+DeepSeek baseline (n=49)

| 程序 | 数据集 | baseline(ms) | 探索期最优 | 正式确认加速比 | status | significant | n | node |
|---|---|---|---|---|---|---|---|---|
| PolyBenchC_no_rag/stencils/jacobi-1d/jacobi-1d.c | polybench | 1.153 | 1.169x | **1.164x** | confirmed | True | 3 | dgx-spark-b-2 |
| CBench_shim_root/cbench-consumer-tiff2rgba_convert/CBench_shim/kernels/consumer_tiff2rgba/consumer_tiff2rgba.c | cbench | 1.992 | 1.085x | **1.031x** | confirmed | True | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/trisolv/trisolv.c | polybench | 9.825 | 1.099x | **1.025x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-consumer-tiff2bw_convert/CBench_shim/kernels/consumer_tiff2bw/consumer_tiff2bw.c | cbench | 0.977 | 1.030x | **1.021x** | confirmed | True | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/durbin/durbin.c | polybench | 2.116 | 1.026x | **1.018x** | confirmed | True | 3 | dgx-spark-b-2 |
| CBench_shim_root/cbench-security-rijndael_decode/CBench_shim/kernels/security_rijndael_decode/security_rijndael_decode.c | cbench | 0.650 | 1.014x | **1.011x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-bzip2_encode/CBench_shim/kernels/bzip2_encode/bzip2_encode.c | cbench | 49.491 | 1.008x | **1.007x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-consumer-tiff2median_convert/CBench_shim/kernels/consumer_tiff2median/consumer_tiff2median.c | cbench | 0.665 | 1.010x | **1.006x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-telecom-crc32_default/CBench_shim/kernels/telecom_crc32/telecom_crc32.c | cbench | 0.598 | 1.010x | **1.003x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-consumer-tiff2dither_convert/CBench_shim/kernels/consumer_tiff2dither/consumer_tiff2dither.c | cbench | 1.520 | 1.015x | **1.002x** | confirmed | True | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-automotive-susan_smoothing/CBench_shim/kernels/automotive_susan_smoothing/automotive_susan_smoothing.c | cbench | 28.163 | 1.005x | **1.001x** | confirmed | True | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/bicg/bicg.c | polybench | 26.045 | 1.034x | **0.999x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-security-rijndael_encode/CBench_shim/kernels/security_rijndael_encode/security_rijndael_encode.c | cbench | 0.609 | 1.370x | **0.998x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-automotive-susan_edges/CBench_shim/kernels/automotive_susan_edges/automotive_susan_edges.c | cbench | 5.030 | 1.012x | **0.998x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-bzip2_decode/CBench_shim/kernels/bzip2_decode/bzip2_decode.c | cbench | 29.403 | 1.001x | **0.997x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-automotive-qsort1_default/CBench_shim/kernels/automotive_qsort1/automotive_qsort1.c | cbench | 8.901 | 1.002x | **0.997x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-network-dijkstra_default/CBench_shim/kernels/network_dijkstra/network_dijkstra.c | cbench | 0.532 | 0.997x | **0.995x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-automotive-susan_corners/CBench_shim/kernels/automotive_susan_corners/automotive_susan_corners.c | cbench | 3.200 | 1.049x | **0.992x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-telecom-adpcm-c_encode/CBench_shim/kernels/telecom_adpcm_c/telecom_adpcm_c.c | cbench | 1.081 | 1.002x | **0.992x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-office-stringsearch2_default/CBench_shim/kernels/office_stringsearch2/office_stringsearch2.c | cbench | 0.634 | 1.021x | **0.979x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-telecom-adpcm-d_decode/CBench_shim/kernels/telecom_adpcm_d/telecom_adpcm_d.c | cbench | 0.757 | 1.020x | **0.978x** | confirmed | False | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/atax/atax.c | polybench | 15.436 | 1.004x | **0.976x** | confirmed | False | 3 | dgx-spark-b-1 |
| CBench_shim_root/cbench-network-patricia_default/CBench_shim/kernels/network_patricia/network_patricia.c | cbench | 0.556 | 1.015x | **0.953x** | confirmed | False | 3 | dgx-spark-b-1 |
| PolyBenchC_no_rag/datamining/correlation/correlation.c | polybench | 1416.284 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-a-2 |
| PolyBenchC_no_rag/linear-algebra/blas/syr2k/syr2k.c | polybench | 1197.509 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-a-1 |
| PolyBenchC_no_rag/medley/nussinov/nussinov.c | polybench | 1598.638 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-a-1 |
| PolyBenchC_no_rag/stencils/fdtd-2d/fdtd-2d.c | polybench | 469.198 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-a-2 |
| CBench_shim_root/cbench-security-sha_default/CBench_shim/kernels/security_sha/security_sha.c | cbench | 0.607 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/datamining/covariance/covariance.c | polybench | 4912.742 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/blas/gemm/gemm.c | polybench | 288.194 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/blas/gemver/gemver.c | polybench | 36.221 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/blas/gesummv/gesummv.c | polybench | 23.585 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/blas/symm/symm.c | polybench | 4053.435 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/blas/syrk/syrk.c | polybench | 393.709 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/blas/trmm/trmm.c | polybench | 542.053 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/2mm/2mm.c | polybench | 1261.774 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/3mm/3mm.c | polybench | 4760.780 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/kernels/doitgen/doitgen.c | polybench | 241.388 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/kernels/mvt/mvt.c | polybench | 18.481 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c | polybench | 6409.994 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/linear-algebra/solvers/gramschmidt/gramschmidt.c | polybench | 1499.477 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-2 |
| PolyBenchC_no_rag/linear-algebra/solvers/ludcmp/ludcmp.c | polybench | 34166.576 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-0 |
| PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c | polybench | 7230.805 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-2 |
| PolyBenchC_no_rag/medley/deriche/deriche.c | polybench | 134.733 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/medley/floyd-warshall/floyd-warshall.c | polybench | 10439.900 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/stencils/adi/adi.c | polybench | 6589.407 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-2 |
| PolyBenchC_no_rag/stencils/heat-3d/heat-3d.c | polybench | 1441.336 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-1 |
| PolyBenchC_no_rag/stencils/jacobi-2d/jacobi-2d.c | polybench | 696.124 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-2 |
| PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c | polybench | 13396.167 | - | N/A(错误结果) | incorrect | False | None | dgx-spark-b-2 |

## 5. 逐程序跨条件对比矩阵

同一个程序在①②OC(和③，若有)几个条件下的加速比对比，用于判断该程序属于"只需改代码" / "需要代码+调参" / "需要完整反馈才行" 哪一类。OC列标注`incorrect`表示该程序OpenCode产出的代码未通过正确性校验，不代表真实加速比。

| 程序(base_id) | ① 只改代码 | ② 代码+调参无反馈 | ③ 完整系统 | OC baseline |
|---|---|---|---|---|
| cb001 | 1.000x | 1.331x | - | 0.997x |
| cb002 | 1.174x | 1.260x | - | 0.992x |
| cb003 | 1.046x | 1.381x | - | 0.998x |
| cb004 | 1.546x | 1.004x | - | 1.001x |
| cb005 | 98.854x | 0.998x | - | 0.997x |
| cb006 | 1.016x | 0.997x | - | 1.007x |
| cb007 | 1.155x | 1.048x | - | 1.021x |
| cb008 | 1.000x | 0.996x | - | 1.002x |
| cb009 | 1.000x | 0.992x | - | 1.006x |
| cb010 | 1.000x | 1.097x | - | 1.031x |
| cb011 | 1.062x | 1.454x | - | 0.995x |
| cb012 | 1.208x | 1.038x | 1.325x | 0.953x |
| cb013 | 1.037x | 1.066x | - | 0.979x |
| cb014 | 1.002x | 1.006x | - | 1.011x |
| cb015 | 1.000x | 1.016x | - | 0.998x |
| cb016 | 1.000x | 1.000x | 1.000x | incorrect |
| cb017 | 1.000x | 1.151x | - | 0.992x |
| cb018 | 1.000x | 1.191x | - | 0.978x |
| cb019 | 0.950x | 1.044x | - | 1.003x |
| pb001 | 11.250x | 9.223x | 13.790x | incorrect |
| pb002 | 14.316x | 15.071x | - | incorrect |
| pb003 | 1.295x | 1.015x | - | incorrect |
| pb004 | 1.588x | 1.174x | - | incorrect |
| pb005 | 1.385x | 1.402x | - | incorrect |
| pb006 | 4.808x | 4.381x | - | incorrect |
| pb007 | 4.029x | 2.995x | - | incorrect |
| pb008 | 2.527x | 1.100x | - | incorrect |
| pb009 | 6.680x | 7.414x | - | incorrect |
| pb010 | 7.037x | 5.480x | - | incorrect |
| pb011 | 5.877x | 4.924x | - | incorrect |
| pb012 | 1.069x | 1.091x | - | 0.976x |
| pb013 | 2.069x | 1.720x | - | 0.999x |
| pb014 | 4.054x | 4.013x | - | incorrect |
| pb015 | 1.242x | 1.182x | - | incorrect |
| pb016 | 1.096x | 1.083x | - | incorrect |
| pb017 | 1.865x | 1.019x | - | 1.018x |
| pb018 | 5.552x | 5.988x | - | incorrect |
| pb019 | 1.309x | 1.078x | - | incorrect |
| pb020 | 1.194x | 1.191x | - | incorrect |
| pb021 | 1.100x | 1.153x | 0.999x | 1.025x |
| pb022 | 2.001x | 1.564x | - | incorrect |
| pb023 | 7.877x | 3.781x | - | incorrect |
| pb024 | 1.305x | 1.109x | - | incorrect |
| pb025 | 3.771x | 1.510x | - | incorrect |
| pb026 | 1.200x | 0.999x | - | incorrect |
| pb027 | 1.446x | 1.022x | - | incorrect |
| pb028 | 1.086x | 1.058x | - | 1.164x |
| pb029 | 1.339x | 1.000x | - | incorrect |
| pb030 | 1.003x | 1.017x | - | incorrect |
