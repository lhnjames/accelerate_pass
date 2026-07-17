# PolyBench 运行结果整理（截至 2026-07-10 17:45 UTC）

## 1. 当前运行状态概览

`run_polybench.sh`（`CONCURRENCY=1`）自 **2026-07-07 19:35** 启动至今仍在运行（PID 716029 → 子进程 852241/852244），已运行约 70 小时。这是对 30 个 PolyBench kernel 的**重新完整跑一遍**（区别于 2026-06-23/24 的上一轮完整结果，见第 3 节）。

- **已完成**：18 / 30
- **正在运行**：ludcmp（自 03:47:10 起，仍在 try_flags 阶段）
- **排队未开始**：11 个（lu, trisolv, deriche, floyd-warshall, nussinov, adi, fdtd-2d, heat-3d, jacobi-1d, jacobi-2d, seidel-2d）— 这些 benchmark 的 `logs/*.log` 里目前仍是 **2026-06-23/24 旧一轮的结果**，尚未被本轮覆盖，不能当作本轮数据引用。

### 已完成 18 个的结果（本轮，2026-07-07 之后）

| Benchmark | Baseline -O3 | 最终最优 speedup | 最优 flags（部分） |
|---|---|---|---|
| correlation | 8528.40 ms | **18.603x** | `-mllvm --slp-max-store-lookup=-1` |
| covariance | 9545.75 ms | **18.555x** | `-mllvm -slp-threshold=10` |
| gemm | 472.64 ms | 1.099x | `-mllvm -partial-unrolling-threshold=100 ...` |
| gemver | 41.11 ms | 4.046x | `-mllvm -pragma-vectorize-memory-check-threshold=128` |
| gesummv | 4.77 ms | 2.557x | `-mllvm -slp-threshold=-8` |
| symm | 5473.28 ms | 11.061x | `-mllvm --slp-max-look-ahead-depth=-4` |
| syr2k | 7399.05 ms | 8.874x | `-mllvm --gvn-max-num-deps=200` |
| syrk | 2167.99 ms | 3.457x | `-mllvm --slp-min-tree-size=2` |
| trmm | 5109.90 ms | 8.528x | `-mllvm --slp-max-store-lookup=0` |
| 2mm | 3463.95 ms | 2.248x | `-mllvm --vectorize-num-stores-pred=128` |
| 3mm | 10280.55 ms | **11.348x** | `-mllvm --slp-min-reg-size=10` |
| atax | 6.53 ms | 1.128x | `-mllvm --slp-threshold=-10 ...` |
| bicg | 10.95 ms | 1.640x | `-mllvm --gvn-hoist-max-depth=50` |
| doitgen | 509.94 ms | 2.877x | `-mllvm --vectorizer-min-trip-count=128` |
| mvt | 31.29 ms | 4.535x | `-mllvm --pragma-vectorize-memory-check-threshold=16` |
| cholesky | 1444.69 ms | 1.250x | `-mllvm --pragma-vectorize-memory-check-threshold=128` |
| durbin | 2.55 ms | 1.703x | `-mllvm --slp-threshold=-8` |
| gramschmidt | 16754.09 ms | **9.321x** | `-mllvm --slp-threshold=-256` |

**Geomean（18 个已完成）：4.159x**（min 1.099x gemm，max 18.603x correlation）

> 与 6 月那一轮相比，本轮 correlation/covariance 从 16.4x/1.09x 大幅提升到 18.6x/18.6x —— 说明 optimize.py 的搜索策略在这期间被迭代过（日志格式也从"Param/Source 分离两行"改成了"参数演化轨迹 + 最优/组合加速比"的统一格式，`run_polybench.sh` 脚本尾部自带的 grep 汇总逻辑仍是旧格式，**已经匹配不上新日志，会输出空值**——这是本文档手动重新解析日志得出结果的原因）。

### 进行中：ludcmp
已跑满 9/9 步的 try_flags 网格搜索阶段（round 里仍在测多个 `-mllvm` 参数），尚未落定最终 confirm 数值，暂无法给出本轮 ludcmp 的最终 speedup。

### 排队中的 11 个
尚未被本轮触及，日志仍是旧数据，预计需等待 ludcmp → lu → trisolv → ... → seidel-2d 依次跑完。按上一轮经验（seidel-2d 单个就耗时数小时到十几小时），全部跑完时间不可预测，可能还需数天。

---

## 2. cBench + TSVC 数据集调用校验

用户要求"检查是否正确调用数据集"，核对结果如下：

| 检查项 | 结果 |
|---|---|
| TSVC manifest（`TSVC_shim/manifest.txt`）条目数 | 151，全部文件路径存在（0 missing） |
| cBench manifest（`CBench_shim_root/manifest.txt`）条目数 | 19，全部文件路径存在（0 missing） |
| 数据集类型自动识别（`src/datasets.py: detect_dataset_type`） | 按路径子串判断：路径含 `tsvc` → `"tsvc"`；含 `cbench` → `"cbench"`；否则查找 PolyBench 目录名 → `"polybench"`。`TSVC_shim/...` 和 `CBench_shim_root/.../CBench_shim/...` 均能正确命中，不会被误判成 polybench |
| utilities/ 头文件定位（`src/polybench_paths.py`） | `TSVC_shim`、`CBench_shim` 已被加入 `POLYBENCH_DIR_NAMES` 白名单，向上遍历目录树能正确找到各自的 `utilities/`（TSVC 是单一 `TSVC_shim/utilities/`；cBench 是每个 benchmark 各自独立的 `CBench_shim_root/<name>/CBench_shim/utilities/`） |
| 实际编译 + 运行冒烟测试（用 `configs/config.yaml` 里配置的 `/usr/bin/clang-11`） | TSVC `s000` 编译成功、运行退出码 0；cBench `telecom_crc32` 编译成功、运行退出码 0 |

**结论：cBench / TSVC 的数据集识别、头文件路径、实际编译运行链路均验证正常，可以放心启动。**

---

## 3. cBench + TSVC 完整运行脚本状态

`run_extra_datasets.sh` 设计为：**先等 `run_polybench.sh` 全部结束，再串行跑 TSVC（151 个）→ 串行跑 cBench（19 个）**，全程 `--rounds 5 --runs 3`，无并发，避免和 PolyBench 那一轮抢 CPU 影响计时噪声。

检查发现该脚本**已经在跑**，不是没启动：

- PID 810291，启动于 **2026-07-08 17:24:48**（`/home/hanning/comet/logs_extra_datasets_master.log` 里对应一条 `waiting for run_polybench.sh to finish...`）
- 目前处于 `wait_for_polybench` 的轮询等待循环里（每 60s 用 `pgrep -f "optimize.py --program PolyBenchC"` 检查一次），因为第 1 节的 PolyBench 全量重跑还没结束，所以它还没真正开始跑 TSVC/cBench
- `logs_tsvc/`、`logs_cbench/` 目前都是空目录，符合"还在等待"的预期

**没有重复启动一个新实例**——因为已有一个正确等待中的实例在跑，重复启动会导致两个进程同时往同一批 log 文件写、`wait_for_polybench` 的 pgrep 判断互相干扰，属于没必要的重复操作。现状是：

1. PolyBench 全量重跑（18/30 完成，ludcmp 进行中，11 个排队）继续跑
2. `run_extra_datasets.sh`（PID 810291）持续每分钟轮询，一旦 PolyBench 结束会自动开始 TSVC（151 个）→ cBench（19 个）串行跑
3. 无需人工干预；如需更快看到 TSVC/cBench 结果，可以考虑手动 kill 掉当前 PolyBench 重跑、直接放行 TSVC/cBench（但这样会丢失 ludcmp 之后 11 个 PolyBench kernel 本轮的重新测试进度）——是否要这样做需要你确认。

---

## 4. 参考：上一轮完整 PolyBench 结果（2026-06-23 19:03 – 06-24 11:37，30/30 全部完成）

详见 `docs/../POLYBENCH_REPORT.md`（本次未改动，仅摘要关键数字，供对比）：

- Geomean speedup：**3.187x**
- 前五名：gramschmidt 16.727x、correlation 16.454x、trmm 13.032x、nussinov 12.533x、doitgen 8.110x
- 完全失败（source 全部 8 轮 precision/compile error）：cholesky、lu、covariance（source 侧）
- 主要问题模式：loop-carried dependency（lu/cholesky/seidel-2d 被 LLM 误并行化）、FP 重排序导致的 precision_error（covariance/gesummv/syr2k 前几轮）、LLM 生成非法 pragma 导致 compile_error（bicg/trmm/doitgen/3mm 后期轮次）

本轮（2026-07-07 起）重跑同一批 30 个 kernel，目的应是复核/改进这些失败案例，目前看 correlation/covariance 已有明显提升（分别到 18.6x），其余 benchmark 需等本轮跑完后再与上表对比。
