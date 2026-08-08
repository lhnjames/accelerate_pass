# 发现：n=3确认对小效应量假阳性率高；未找到"代码重写+编译flag两个维度都稳定生效"的单一case

日期：2026-07-27。起因：用户要求找一个"改代码有效、调flag也有明显且稳定收益、两者组合也work"的case study。
经过对30个PolyBench kernel + 若干cBench程序、6种flag策略的系统性搜索和n=9严格复测，**没有找到满足全部条件的case**，
但过程中发现了一个更重要、影响面更广的问题：**comet默认的n=3交替确认，对10%~30%量级的"小效应"有相当高的假阳性率**。

## 1. 结论

在本次搜索覆盖的kernel里，**每一个都只有一个真正稳定的瓶颈维度**：要么代码重写起决定性作用（flag贡献在n=9下collapse成噪声），
要么编译flag起决定性作用（代码重写贡献在n=9下collapse成噪声），从未观察到两个维度同时、独立、稳定地都有实质贡献。

| kernel | 稳定的维度 | 该维度n=9确认加速比 | 崩溃的维度 | 该维度n=3官方结果 | 该维度n=9复测结果 |
|---|---|---|---|---|---|
| 2mm | 代码重写 | **8.883x**（IQR[8.607,8.927], n_positive=9/9） | 编译flag | 1.2449x, **significant=True**, IQR[1.2344,1.2449] (cv=0.2%!) | 1.007x, n_positive=5/9, IQR[0.962,1.073]（噪声） |
| durbin | 编译flag(`-ffast-math`) | **1.392x**（IQR[1.379,1.413], n_positive=9/9） | 代码重写 | 1.8647x, **significant=True**, IQR[0.4382,1.8647] (cv=32~40%) | 1.007x, n_positive=5/9, IQR[0.983,1.018]（噪声） |
| automotive_susan_smoothing (cBench) | 编译flag(`-flto`) | **1.281x**（IQR[1.276,1.294], n_positive=9/9） | 代码重写 | 1.5458x, significant=**False**（n=3时已标记不显著）, IQR[0.9950,1.5458] (cv=28.3%) | 0.998x, n_positive=3/9, IQR[0.995,1.000]（噪声，与n=3已给出的"不显著"一致，但点估计1.5458x仍严重偏高） |
| syrk | 代码重写 | **3.054x**（IQR[2.918,3.332], n_positive=9/9） | 编译flag（官方选择：`-licm-max-num-uses-traversed=32`） | 1.0812x, **significant=True** | 0.945x, n_positive=3/9, IQR[0.883,1.018]（噪声） |
| syrk | 同上 | 同上 | 编译flag（我自选：`-funroll-loops -mprefer-vector-width=512`筛选值） | 1.217x（单次筛选，未经n=3官方确认） | 0.929x, n_positive=4/9, IQR[0.865,1.067]（噪声） |

**最关键的证据是2mm**：官方n=3确认给出的IQR是[1.2344, 1.2449]，跨度极窄，base_cv/best_cv都只有0.2%——**表面上看起来是一个噪声极低、非常可信的"显著"结果**，但用n=9独立复测后彻底崩溃为噪声(1.007x)。这说明**仅凭n=3自身报告的IQR窄/cv低，并不能保证该显著性不是假阳性**——低n下即使测量本身"看起来很稳"，也可能只是恰好3次抽样都落在了同一侧噪声区间里。

## 2. 完整搜索方法记录

### 2.1 `-ffast-math`（全部30个PolyBench kernel + 3个cBench程序）
覆盖：correlation, covariance, gesummv, durbin, adi, heat-3d, jacobi-1d, jacobi-2d, seidel-2d, fdtd-2d,
cholesky, ludcmp, lu, trisolv, gramschmidt, symm, syrk, syr2k, trmm, 2mm, 3mm, doitgen, gemm, gemver,
atax, bicg, mvt, nussinov, deriche, floyd-warshall（PolyBench全部30个）；
automotive_susan_smoothing, consumer_tiff2bw, network_patricia（cBench 3个）。

结果：仅durbin(1.54x)、seidel-2d(2.38x，仅n=2筛选，未做n=9严格复测)明显。其余全部在0.7x~1.07x之间（多数≈1.0x或更差，如deriche 0.722x、adi 0.932x）。cBench三个均≈1.0x（这批程序以整数/字节操作为主，几乎不含浮点归约，`-ffast-math`原理上就不该有效果）。

### 2.2 `-funroll-loops -mprefer-vector-width=512`（9个kernel）
覆盖：gramschmidt, trmm, 3mm, symm, adi, syrk, bicg, syr2k, doitgen。
结果：仅syrk(1.217x)明显，n=9严格复测后崩溃为0.929x（噪声）。其余均在0.98x~1.02x之间。

### 2.3 `-march=native` / `-flto` / 两者组合（14个PolyBench kernel + 3个cBench程序）
PolyBench覆盖：trmm, 3mm, gramschmidt, symm, syr2k, doitgen, adi, syrk, bicg, deriche, gemver, heat-3d,
correlation, floyd-warshall。结果：全部在0.96x~1.08x之间，无一显著（bicg的marchlto组合1.080x、gemver的march
1.084x是最高的两个，量级本身就已经落在"很可能是噪声"的区间，未再耗费算力单独验证）。

cBench覆盖：automotive_susan_smoothing, consumer_tiff2bw, network_patricia，每个都试了march/lto/fnsi/unroll/组合
共5种。仅susan_smoothing+`-flto`(1.277x)明显，n=9复测后确认为真实、稳定的1.281x。

### 2.4 comet官方condition④（params-only，9轮LLM搜索出的flag组合）复测
对2mm（`-partial-unrolling-threshold=30`）和syrk（`-licm-max-num-uses-traversed=32`）两个官方n=3
"significant=True"结果做了n=9独立复测，两个都崩溃为噪声，说明这不只是"我随手试的flag运气不好"，
comet自己系统性搜索出的flag组合同样存在这个问题。

## 3. 两个曾经误判、已撤回的"bug"（记录在案，避免重复踩坑）

### 3.1 2mm"83%数值错误" —— 已撤回，源码本身正确
最初误以为2mm的代码重写（restrict+分块）在`-DPOLYBENCH_TIME`构建下于LARGE_DATASET算错了83%。
根因排查后发现：**PolyBench在`-DPOLYBENCH_TIME`模式下，`polybench_print_instruments`只打印墙钟耗时，
不打印数组内容**（`polybench.h`第208-214行：`#define polybench_print_instruments polybench_timer_print();`）。
所以当时的"correctness"比较实际上比较的是两次运行的耗时数字，跟计算结果是否正确毫无关系。用正确的方法
（`-DPOLYBENCH_DUMP_ARRAYS`构建，比较真实数组输出）重新验证后，2mm的代码重写在LARGE_DATASET下完全正确。

### 3.2 durbin"Levinson-Durbin数值病态" —— 已撤回，同样是上面这个bug的另一个受害者
最初用同一二进制自己跟自己比较（`-DPOLYBENCH_TIME`构建），发现"结果不一致"，误判为durbin递归算法
本身在LARGE_DATASET下有数值病态。根因同3.1：比较的是两次运行的计时噪声，不是真实计算结果。用
`-DPOLYBENCH_DUMP_ARRAYS`重新验证后，durbin自己跟自己完全一致，三个候选（flags/rewrite/both）在
LARGE_DATASET下也都正确。

**教训**：任何"验证正确性"的工作，必须确认比较的是`-DPOLYBENCH_DUMP_ARRAYS`（或等价的真实数组转储）构建的
输出，而不是`-DPOLYBENCH_TIME`构建的输出——后者的标准输出只有一个计时数字，拿它做"数值比较"在方法论上
从一开始就是错的，不管epsilon设多严都没用。

## 4. 对151任务消融数据可信度的影响评估

1. **条件④（纯调参，无代码改动）的可信度需要整体打折扣**。④号条件目前已完成的18个结果里，
   多数加速比落在1.00x~1.24x区间——正是本次调查证明"极易在n=3下产生假阳性"的量级。已经确认崩溃的两个
   案例（2mm的1.2449x、syrk的1.0812x）都在这个区间内。**不能假设④号条件里"significant=True"的结果
   就是真实的**，除非用n≥9独立复测。

2. **条件①②③里的大幅加速（>2x）看起来是可信的**。本次复测的两个大幅加速案例（2mm的rewrite-only 7-9x区间、
   syrk的rewrite-only 2.5-3x区间）在n=9下都维持了同量级的显著效果，说明报告里那些10x、15x级别的headline
   数字大概率是真实的，不是测量噪声。

3. **之前写的两份motivation example文档里"flags在rewrite基础上还贡献了X%"的具体数字需要加免责声明**：
   - `MOTIVATION_EXAMPLE_covariance.md`里"flags独立贡献了约4%"（9.916x vs 9.521x等）
   - 之前口头提到的correlation"某一步flags贡献了43%"（6.460x纯源码 vs 9.243x+flags）
   这些都只是log里单次"交替验证"报告出来的差值，从未用n=9独立复测过，按本次发现的模式，**这类10-40%的
   "flags在rewrite上的增量贡献"有相当概率本身就是噪声**，在论文里引用前需要重新用n≥9验证。

4. **不应笼统地说"flags完全没用"**——durbin的`-ffast-math`(1.39x)和susan_smoothing的`-flto`(1.28x)
   都是在n=9下稳定复现的真实效果，只是这两个kernel恰好是"flag起决定性作用、代码重写不起作用"的反例，
   而不是"两者都起作用"的正例。

## 5. 建议

1. **把默认confirm的`--runs`从3提到至少9**，尤其是当探索期观测到的效应量落在1.0x~1.5x区间时——本次
   数据显示>2x的效应在n=3下就相当稳定，但<50%的效应必须n≥9才能可靠区分真实效果与噪声。
2. **"significant"判定不能只看点估计的符号**，应该把IQR宽度/cv%也纳入判定条件——2mm的官方n=3结果IQR
   窄到cv=0.2%依然是假阳性，说明"n=3看起来很稳"本身不是可靠的信号，必须提高样本量而非依赖低方差表象。
3. 对已发布的151任务数据，建议给条件④和所有<1.5x的结果统一标注"未经高n复测，置信度低"的提示，
   而不是直接采信其"significant=True"标签。
4. 后续如果论文确实需要一个"两个维度都独立生效"的case study，可能需要放宽"必须来自现有51个kernel"
   的限制，主动设计/选择一个已知同时对代码结构和编译参数都敏感的更大规模程序（例如手写一个更复杂的
   多阶段kernel），而不是依赖现有PolyBench/cBench语料里恰好存在这样的例子——现有证据表明，在这批
   benchmark的规模和结构下，"单一瓶颈"可能是常态而非例外。
