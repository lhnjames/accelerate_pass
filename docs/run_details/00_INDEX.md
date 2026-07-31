# COMET 消融实验 — 完整过程记录索引

_生成时间：2026-07-31_

本目录每个文件对应一个实验条件，内含该条件下所有**已完成**任务的完整原始日志（逐步 Action / Reasoning / 结果 / 灾难性退化回退记录等），供逐条人工核查。

## ⚠️ 数据有效性说明

**PO（AutoPass）条件的历史结果已全部作废并正在重跑。** 原因是发现三个测量bug（详见 `docs/AUTOPASS_REPRODUCTION.md`）：codegen 误用 -O0、frontend 误用 -O1、以及拿几小时前测的 stale baseline 做分母。三者叠加使每个候选都被系统性低估到 0.2x–0.7x。修复后 harness 在 gemm 上能精确复现 -O3（1.0022x）。**本文件中 PO 行若仍有旧数据，不可引用。**

其余条件（①②③④/OC）不受这三个 bug 影响——它们不走 passorder harness。

## 总体进度

- 总任务数：261
- 已完成：198
- 未完成（pending+running）：63

| 条件 | 已完成 | geomean | 中位数 | 未完成 | 详情 |
|---|---|---|---|---|---|
| ① rewrite-only（禁用编译反馈，每步强制 rewrite_source） | 49 | 1.905x | 1.242x | 2 | [01_condition1_rewrite_only.md](01_condition1_rewrite_only.md) |
| ② no-compiler-feedback（自由选择动作，屏蔽编译器反馈） | 49 | 1.556x | 1.078x | 2 | [02_condition2_no_feedback.md](02_condition2_no_feedback.md) |
| ③ full system（自由选择 + 完整编译反馈） | 4 | 2.061x | 1.210x | 2 | [03_condition3_full.md](03_condition3_full.md) |
| ④ params-only（每步强制 try_flags） | 45 | 1.101x | 1.030x | 4 | [04_condition4_params_only.md](04_condition4_params_only.md) |
| OC：OpenCode + DeepSeek 外部 CLI agent baseline | 49 | 1.003x | 1.000x | 2 | [05_opencode_deepseek.md](05_opencode_deepseek.md) |
| PO：AutoPass (arxiv 2606.20373) 四-agent 复现 baseline ⚠️作废重跑中 | 0 | — | — | 51 | [06_passorder_autopass.md](06_passorder_autopass.md) |

## 未完成任务明细

### ① rewrite-only（禁用编译反馈，每步强制 rewrite_source） — 2 个
- `c1_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `c1_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c

### ② no-compiler-feedback（自由选择动作，屏蔽编译器反馈） — 2 个
- `c2_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `c2_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c

### ③ full system（自由选择 + 完整编译反馈） — 2 个
- `c3_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `c3_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c

### ④ params-only（每步强制 try_flags） — 4 个
- `c4_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `c4_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c
- `c4_pb016` [pending] PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c
- `c4_pb030` [pending] PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c

### OC：OpenCode + DeepSeek 外部 CLI agent baseline — 2 个
- `oc_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `oc_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c

### PO：AutoPass (arxiv 2606.20373) 四-agent 复现 baseline — 51 个
- `po_cb001` [pending] CBench_shim_root/cbench-automotive-qsort1_default/CBench_shim/kernels/automotive_qsort1/automotive_qsort1.c
- `po_cb002` [pending] CBench_shim_root/cbench-automotive-susan_corners/CBench_shim/kernels/automotive_susan_corners/automotive_susan_corners.c
- `po_cb003` [pending] CBench_shim_root/cbench-automotive-susan_edges/CBench_shim/kernels/automotive_susan_edges/automotive_susan_edges.c
- `po_cb004` [pending] CBench_shim_root/cbench-automotive-susan_smoothing/CBench_shim/kernels/automotive_susan_smoothing/automotive_susan_smoothing.c
- `po_cb005` [pending] CBench_shim_root/cbench-bzip2_decode/CBench_shim/kernels/bzip2_decode/bzip2_decode.c
- `po_cb006` [pending] CBench_shim_root/cbench-bzip2_encode/CBench_shim/kernels/bzip2_encode/bzip2_encode.c
- `po_cb007` [pending] CBench_shim_root/cbench-consumer-tiff2bw_convert/CBench_shim/kernels/consumer_tiff2bw/consumer_tiff2bw.c
- `po_cb008` [pending] CBench_shim_root/cbench-consumer-tiff2dither_convert/CBench_shim/kernels/consumer_tiff2dither/consumer_tiff2dither.c
- `po_cb009` [pending] CBench_shim_root/cbench-consumer-tiff2median_convert/CBench_shim/kernels/consumer_tiff2median/consumer_tiff2median.c
- `po_cb010` [pending] CBench_shim_root/cbench-consumer-tiff2rgba_convert/CBench_shim/kernels/consumer_tiff2rgba/consumer_tiff2rgba.c
- `po_cb011` [pending] CBench_shim_root/cbench-network-dijkstra_default/CBench_shim/kernels/network_dijkstra/network_dijkstra.c
- `po_cb012` [pending] CBench_shim_root/cbench-network-patricia_default/CBench_shim/kernels/network_patricia/network_patricia.c
- `po_cb013` [pending] CBench_shim_root/cbench-office-stringsearch2_default/CBench_shim/kernels/office_stringsearch2/office_stringsearch2.c
- `po_cb014` [pending] CBench_shim_root/cbench-security-rijndael_decode/CBench_shim/kernels/security_rijndael_decode/security_rijndael_decode.c
- `po_cb015` [pending] CBench_shim_root/cbench-security-rijndael_encode/CBench_shim/kernels/security_rijndael_encode/security_rijndael_encode.c
- `po_cb016` [pending] CBench_shim_root/cbench-security-sha_default/CBench_shim/kernels/security_sha/security_sha.c
- `po_cb017` [pending] CBench_shim_root/cbench-telecom-adpcm-c_encode/CBench_shim/kernels/telecom_adpcm_c/telecom_adpcm_c.c
- `po_cb018` [pending] CBench_shim_root/cbench-telecom-adpcm-d_decode/CBench_shim/kernels/telecom_adpcm_d/telecom_adpcm_d.c
- `po_cb019` [pending] CBench_shim_root/cbench-telecom-crc32_default/CBench_shim/kernels/telecom_crc32/telecom_crc32.c
- `po_cb020` [pending] CBench_shim_root/cbench-security-blowfish_encode/CBench_shim/kernels/security_blowfish_encode/security_blowfish_encode.c
- `po_cb021` [pending] CBench_shim_root/cbench-security-blowfish_decode/CBench_shim/kernels/security_blowfish_decode/security_blowfish_decode.c
- `po_pb001` [running] PolyBenchC_no_rag/datamining/correlation/correlation.c
- `po_pb002` [running] PolyBenchC_no_rag/datamining/covariance/covariance.c
- `po_pb003` [pending] PolyBenchC_no_rag/linear-algebra/blas/gemm/gemm.c
- `po_pb004` [pending] PolyBenchC_no_rag/linear-algebra/blas/gemver/gemver.c
- `po_pb005` [pending] PolyBenchC_no_rag/linear-algebra/blas/gesummv/gesummv.c
- `po_pb006` [pending] PolyBenchC_no_rag/linear-algebra/blas/symm/symm.c
- `po_pb007` [pending] PolyBenchC_no_rag/linear-algebra/blas/syr2k/syr2k.c
- `po_pb008` [pending] PolyBenchC_no_rag/linear-algebra/blas/syrk/syrk.c
- `po_pb009` [pending] PolyBenchC_no_rag/linear-algebra/blas/trmm/trmm.c
- `po_pb010` [pending] PolyBenchC_no_rag/linear-algebra/kernels/2mm/2mm.c
- `po_pb011` [pending] PolyBenchC_no_rag/linear-algebra/kernels/3mm/3mm.c
- `po_pb012` [pending] PolyBenchC_no_rag/linear-algebra/kernels/atax/atax.c
- `po_pb013` [pending] PolyBenchC_no_rag/linear-algebra/kernels/bicg/bicg.c
- `po_pb014` [pending] PolyBenchC_no_rag/linear-algebra/kernels/doitgen/doitgen.c
- `po_pb015` [pending] PolyBenchC_no_rag/linear-algebra/kernels/mvt/mvt.c
- `po_pb016` [pending] PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c
- `po_pb017` [pending] PolyBenchC_no_rag/linear-algebra/solvers/durbin/durbin.c
- `po_pb018` [pending] PolyBenchC_no_rag/linear-algebra/solvers/gramschmidt/gramschmidt.c
- `po_pb019` [pending] PolyBenchC_no_rag/linear-algebra/solvers/ludcmp/ludcmp.c
- `po_pb020` [pending] PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c
- `po_pb021` [pending] PolyBenchC_no_rag/linear-algebra/solvers/trisolv/trisolv.c
- `po_pb022` [pending] PolyBenchC_no_rag/medley/deriche/deriche.c
- `po_pb023` [pending] PolyBenchC_no_rag/medley/floyd-warshall/floyd-warshall.c
- `po_pb024` [pending] PolyBenchC_no_rag/medley/nussinov/nussinov.c
- `po_pb025` [pending] PolyBenchC_no_rag/stencils/adi/adi.c
- `po_pb026` [pending] PolyBenchC_no_rag/stencils/fdtd-2d/fdtd-2d.c
- `po_pb027` [pending] PolyBenchC_no_rag/stencils/heat-3d/heat-3d.c
- `po_pb028` [pending] PolyBenchC_no_rag/stencils/jacobi-1d/jacobi-1d.c
- `po_pb029` [pending] PolyBenchC_no_rag/stencils/jacobi-2d/jacobi-2d.c
- `po_pb030` [pending] PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c
