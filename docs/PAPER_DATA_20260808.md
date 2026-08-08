# COMET 论文数据全集 — 每任务 / 每步骤 / 每效果

_生成时间：2026-08-08 05:53 UTC，由 `scripts/gen_paper_data.py` 自动生成_

本文件是完整实验记录：每个任务的基线、自动判定的正确性档位、agent 每一步做了什么、该步实测多少、被拒候选及原因、以及最终配对确认。论文里任何一个数字都可以在这里回溯到产生它的那一步。

## 1. 有效性保证

### 1.1 正确性门的变异测试

方法：把 kernel 函数体替换为 `return;`（等于什么都不算），重新编译并送入正确性检查。**门必须判失败**；判通过说明该 benchmark 无法校验任何计算错误。全部 30 个 PolyBench 程序逐个测试。

| 打印精度 | 门有效 | 门无效 |
|---|---:|---|
| `%0.2lf`（原始，量化步长 1e-2） | 29/30 | heat-3d |
| `%0.2lf` + 量化容差 | 27/30 | heat-3d、**jacobi-1d**、**seidel-2d**（回归） |
| **`%0.12lf`（现用，量化步长 1e-12）** | **28/30** | heat-3d、seidel-2d |

结论：提高打印精度同时解决了两个方向的问题——向量化重结合（~1e-13 相对）不再被误判，而清空 kernel 仍被检出。仅放宽容差则会让 jacobi-1d、seidel-2d 的门失效，是错误的取舍。

### 1.2 必须排除的程序

| 程序 | 原因 |
|---|---|
| `automotive_qsort1` | 输出仅 59 字节标题行，排序结果从不打印；把比较函数改成恒返回 0，输出逐字节相同 |
| `consumer_tiff2median` | 程序报 'Not a b&w image.' 直接退出，46 字节，从未执行计算 |
| `heat-3d` | dump 的数组与 kernel 完全无关，清空 kernel 后输出逐位相同（1e-12 精度下仍然如此） |
| `seidel-2d` | 同上，任何打印精度下都无法检出 kernel 被清空 |

### 1.3 数据排除规则（按时间界定，修复后重跑的数据不受影响）

| 类别 | 含义 |
|---|---|
| 正确性档位过松 | 该程序判定 2026-08-02 从 `numeric`（1e-4 **相对**容差）收紧为 `hash`（逐字节）。telecom_crc32 打印 ~4e9 校验和，1e-4 相对容差允许 ±40 万误差 |
| 孤儿抢核 | 两个 PPID=1 的遗留进程各霸占 pin 核 20+ 小时，与 worker 争抢同一 CPU |
| PO 预算被吞 | InstCombine fixpoint 校验器 abort 掉 147 轮中的 19 轮，4 个程序三轮全废却记为 1.000x |

## 2. 总体结果

加速比 = n 次交替配对测量比值的**中位数**（不是最大值）。`显著` 要求 IQR 整体位于 1.0 之上且每次配对均为正。

### 2.1 PolyBench（已剔除无效数据）

| 条件 | n | geomean | 中位数 | 最小 | 最大 |
|---|---:|---:|---:|---:|---:|
| ① rewrite-only | 28 | **2.2860** | 1.4849 | 0.8856 | 16.3119 |
| ② no-compiler-feedback | 28 | **2.1698** | 1.5568 | 0.9687 | 18.8261 |
| ③ full system | 28 | **2.4722** | 1.5900 | 0.8333 | 17.1274 |
| ④ params-only | 28 | **1.0290** | 1.0074 | 0.9625 | 1.2375 |
| OC | 28 | **1.1494** | 1.0000 | 0.9759 | 6.5494 |
| PO | 28 | **1.0221** | 1.0015 | 0.9897 | 1.1970 |

**配对符号检验**（同一程序两条件都有数据）：

| 对比 | n | 胜 | 负 | 前者 geomean | 后者 geomean | p | 结论 |
|---|---:|---:|---:|---:|---:|---:|---|
| ③ full system vs ② no-compiler-feedback | 28 | 18 | 10 | 2.4722 | 2.1698 | 0.1849 | 不显著 |
| ③ full system vs ① rewrite-only | 28 | 18 | 10 | 2.4722 | 2.2860 | 0.1849 | 不显著 |
| ① rewrite-only vs ② no-compiler-feedback | 28 | 17 | 11 | 2.2860 | 2.1698 | 0.3449 | 不显著 |
| ③ full system vs ④ params-only | 28 | 24 | 4 | 2.4722 | 1.0290 | 0.0002 | **显著** |
| ③ full system vs PO | 28 | 27 | 1 | 2.4722 | 1.0221 | 0.0000 | **显著** |
| ③ full system vs OC | 28 | 25 | 3 | 2.4722 | 1.1494 | 0.0000 | **显著** |

### 2.2 cBench（已剔除无效数据）

| 条件 | n | geomean | 中位数 | 最小 | 最大 |
|---|---:|---:|---:|---:|---:|
| ① rewrite-only | 19 | **1.0087** | 1.0000 | 0.9684 | 1.1674 |
| ② no-compiler-feedback | 19 | **1.0105** | 1.0032 | 0.9888 | 1.1170 |
| ③ full system | 19 | **1.0147** | 1.0041 | 0.9840 | 1.1664 |
| ④ params-only | 19 | **1.0078** | 1.0029 | 0.9905 | 1.0472 |
| OC | 19 | **1.0105** | 1.0005 | 0.9734 | 1.1096 |
| PO | 19 | **0.9960** | 0.9978 | 0.9602 | 1.0056 |

**配对符号检验**（同一程序两条件都有数据）：

| 对比 | n | 胜 | 负 | 前者 geomean | 后者 geomean | p | 结论 |
|---|---:|---:|---:|---:|---:|---:|---|
| ③ full system vs ② no-compiler-feedback | 19 | 10 | 9 | 1.0147 | 1.0105 | 1.0000 | 不显著 |
| ③ full system vs ① rewrite-only | 19 | 12 | 6 | 1.0147 | 1.0087 | 0.2379 | 不显著 |
| ① rewrite-only vs ② no-compiler-feedback | 19 | 6 | 11 | 1.0087 | 1.0105 | 0.3323 | 不显著 |
| ③ full system vs ④ params-only | 19 | 10 | 9 | 1.0147 | 1.0078 | 1.0000 | 不显著 |
| ③ full system vs PO | 19 | 15 | 4 | 1.0147 | 0.9960 | 0.0192 | **显著** |
| ③ full system vs OC | 19 | 7 | 12 | 1.0147 | 1.0105 | 0.3593 | 不显著 |

## 2.3 编译器信息驱动的参数调整，实际带来多少

这一节单独回答"编译器反馈起了什么作用"。做法是把 `try_flags` 步骤最终采纳的 `-mllvm` 选项原样取出，**只对原始源码**单独编译，在空闲核上与 -O3 基线交替配对测量，并逐字节比对 DUMP_ARRAYS 输出。测的是纯参数的净效果，不含任何源码改写。

> **一个必须避开的陷阱**：日志里的 `步骤N: X.XXXx` 是**当时的累计最优**，不是该步增量。correlation 的 `步骤2: 11.316x [rewrite]` 之后 `步骤3: 11.231x [flags]`——flags 那步实际是 **−0.085**。按步直接归因会把源码重写的功劳算到参数头上。下表的"增量"一律用 `该步累计 − 此前最优` 计算。

### 探索期看起来 >1.5x 的参数，全部不成立

| 程序 | 采纳的 -mllvm 选项 | 探索期读数 | **独立复测** | 正向 |
|---|---|---:|---:|---:|
| consumer_tiff2rgba | `-vectorize-memory-check-threshold=256 -slp-max-vf=8` | 6.542 | **1.0112** | 6/11 |
| telecom_crc32 | `-licm-max-num-uses-traversed=16` | 4.229 | **0.9628** | 1/11 |
| office_stringsearch2 | `--unroll-max-upperbound=64` | 2.094 | **0.9988** | 5/11 |
| network_patricia | `--licm-max-num-int-reassociations=32` | 1.988 | **1.0224** | 9/11 |
| floyd-warshall | `-unroll-threshold=1500 -vectorizer-min-trip-count=1` | 2.640 | **0.9997** | 3/11 |
| jacobi-1d | `-unroll-threshold=1000` | 1.594 | **0.9942** | 5/11 |

六个全部塌回 1.0 附近，输出逐字节一致——**塌掉的是加速比，不是正确性**。这些读数产生于探索期的单次测量，没有经过配对确认门。

### 通过确认门的参数结果：真实但幅度有限

| 程序 | 采纳的 -mllvm 选项 | 报告确认值 | **独立复测** | 正向 | 复测 IQR |
|---|---|---:|---:|---:|---|
| 2mm | `-partial-unrolling-threshold=30` | 1.2375 | **1.1139** | 8/9 | [1.1106, 1.1239] |
| jacobi-2d | `--partial-unrolling-threshold=256` | 1.1735 | **1.0989** | 9/9 | [1.0982, 1.1005] |
| adi | `--partial-unrolling-threshold=32 --instcombine-guard-widening-window=64 --unroll-max-iteration-count-to-analyze=64` | 1.0714 | **1.0787** | 8/9 | [1.0268, 1.1100] |
| lu | `-partial-unrolling-threshold=8000` | 1.1028 | **1.0135** | 8/9 | [1.0065, 1.0163] |
| durbin | `-vectorize-memory-check-threshold=32` | 1.0537 | **0.9925** | 2/9 | [0.9875, 0.9999] |
| heat-3d | `-partial-unrolling-threshold=4000 -slp-max-vf=4` | 1.0623 | **0.8708** | 0/9 | [0.8555, 0.8719] |

**结论：编译器参数调整确实有真实收益，但天花板很低。**经独立复测站得住的最大值是 2mm 的 **1.1139x**（`-mllvm -partial-unrolling-threshold=30`，8/9 正向，IQR 宽度仅 1.2%），其次 jacobi-2d 1.0989x（9/9，IQR ±0.1%）与 adi 1.0787x。**整个语料里没有任何一个经验证的 >1.5x 纯参数案例。**

两个不成立的：`durbin` 基线只有 2.2 ms，落在测不出来的区间；`heat-3d` 复测为 0.8708（0/9 正向），而它本来就因正确性门无效被排除。

与条件 ④ 的整体结果一致：PolyBench 上 ④ 的 geomean 是 1.029，确认值上限 1.2375。参数通道的贡献是**个位数百分比**，而源码重写通道是 2.2–2.5 倍。

## 3. 逐任务逐步明细

每步格式：`步骤N: <该步实测加速比>x [<动作>: <具体做了什么>]`，失败步记录被拒原因。这是 agent 的完整决策轨迹。

### ① rewrite-only（每步强制 rewrite_source，屏蔽编译器反馈）（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>6.9811x</b>（基线 1082.42 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb010`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：7.2387x　最终确认：**6.9811x**　IQR [6.4967, 7.0366]　base_cv=3.9% best_cv=0.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 4.888x  [rewrite: 对第一个矩阵乘循环交换 j 和 k 的嵌套顺序：将 for(j) 移到最内层，使原内层循环 for(k) 成为中间层。这样 B[k][j] 访问变为连续（j 连续），A[i][k] 提升到 j 循环外。类似地，对第二个矩阵乘循环交换 j 和 k，使 C[k][j] 访问连续，tmp[i][k] 提升。不改变累加语义但允许浮点顺序变化。] |
| 2 | rewrite_source | 7.038x  [rewrite: 对两个矩阵乘实施三维 loop tiling（i, k, j）。第一个矩阵乘（tmp = α*A*B）在原 i,k,j 嵌套外加 ii, kk, jj 块循环，块尺寸初步设为 BI=64, BK=32, BJ=64，使 A[BI][BK]、B[BK][BJ]、tmp[BI][BJ] 三个块总数据量保持在 L1 缓存内。第二个矩阵乘（D += tmp*C）同样对 i, k（此时 k 遍历 NJ）、j 施加类似分块，块尺寸 BI2=64, BK2=32, BJ2=64。注意第二个乘的索引空间：i < NI，k < NJ，j < NL，分块参数复用思路但边界独立。不改变浮点累加顺序（内层仍为 j 循环连续访问，保持 current_best 的顺序），仅通过块边界限制临时数组块尺寸。] |
| 3 | rewrite_source | 5.934x  [rewrite: 在两个矩阵乘的每个tile内引入临时累加缓冲区：对第一个乘(tmp=alpha*A*B)，在(ii,jj)块内，对每个i分配局部数组sum_buf[BJ]并置零，然后遍历kk块内的k，将aik*B[k][j]累加到sum_buf[j]，最后将sum_buf加到tmp[i][j]。对第二个乘(D+=tmp*C)采用相同模式：在(ii2,jj2)块内，对每个i分配sum_buf2[BJ2]置零，遍历kk2块内k，累加tmp_ik*C[k][j]到sum_buf2[j]，最后加到D[i][j]。缓冲区大小与当前tile的BJ保持一致，确保每个缓冲区仅占512字节，可完全驻留在L1缓存中，消除对tmp和D的重复读写。] |
| 4 | rewrite_source | 4.834x  [rewrite: Swap the tile loop order in both matrix multiplies from (ii, kk, jj) to (ii, jj, kk) (i.e., move jj tile loop outside kk tile loop), keeping inner loops (i, k, j) unchanged. This reuses the B tile across all kk iterations for a fixed jj, improving temporal locality.] |
| 5 | rewrite_source | 7.239x  [rewrite: Add the restrict keyword to all matrix pointer parameters (tmp, A, B, C, D) in kernel_2mm's signature to guarantee no aliasing between the arrays. Simultaneously increase the tile sizes to BI=128, BK=64, BJ=128 (and BI2=128, BK2=64, BJ2=128) to raise the arithmetic intensity per tile while keeping the total working set (~512 KB) within the 1.3 MiB L1 cache, reducing loop overhead |
| 6 | rewrite_source | 6.339x  [rewrite: Fuse the zero-initialization of tmp into the first matrix multiply tiling loop by initializing each tile's portion of tmp to zero before the inner k loops, eliminating the separate full-array zeroing loop. Similarly, fuse the D *= beta scaling into the second matrix multiply tiling loop by applying beta to each D element within the tile before accumulating the product, eliminatin |
| 7 | rewrite_source | 4.755x  [rewrite: 在 tile 内部将二维数组访问替换为 restrict 行指针（如 tmp_i = tmp_r[i], B_k = B_r[k] 等），以减少最内层 j 循环中的地址计算；同时将第一、二矩阵乘的 tile 尺寸调整为 BI=128, BK=256, BJ=64（及对应的 BI2/BK2/BJ2），使 B 的列块增大以延长每次 k 迭代的计算密度，且总工作集仍低于 L1d 容量。] |
| 8 | rewrite_source | 6.046x  [rewrite: Increase the tile sizes for the first matrix multiply to BI=256, BK=64, BJ=128 and for the second to BI2=256, BK2=64, BJ2=128, to increase arithmetic intensity per tile iteration, reduce outer-loop overhead, and keep total working set (~512 KB) well within the 1.3 MiB L1d cache.] |
| 9 | rewrite_source | 5.565x  [rewrite: 在第一个矩阵乘的内层 j 循环中手动展开 4 倍，引入 4 个标量累加器 tmp_i_j0..tmp_i_j3，分别累加 B_k[j+0..3]*aik，最后一次性写回 tmp_i[j..j+3]。同理在第二个矩阵乘的内层 j 循环也手动展开 4 倍，使用 D_i_j0..D_i_j3 累加器。保持 tile 尺寸不变，准确处理尾部非 4 整除边界。] |

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>5.3201x</b>（基线 1601.16 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb011`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：5.9006x　最终确认：**5.3201x**　IQR [4.9738, 5.8773]　base_cv=13.3% best_cv=5.0%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 5.529x  [rewrite: 对三个矩阵乘法循环（E:=A*B, F:=C*D, G:=E*F）进行循环交换：将最内层 k 循环与外层 j 循环交换，使 j 成为最内层循环、k 成为中层循环（ikj 顺序）。这样 B[k][j]、D[k][j]、F[k][j] 的访问变为连续，消除跨步访问，提升缓存命中率和向量化能力。数值顺序不变，保持精度一致。] |
| 2 | rewrite_source | 5.901x  [rewrite: 对三个矩阵乘法分别应用loop tiling：将i循环和j循环各分成大小为TILE_SIZE的块（如64），形成i_tile和j_tile两层外循环，内层保持原ikj顺序。具体为：E:=A*B部分添加for(i_tile)和for(j_tile)，内嵌原i、k、j循环范围限定在tile内；F:=C*D和G:=E*F类似。这样每个tile内数据可完全放入L1/L2 cache，大幅降低DRAM流量。] |
| 3 | rewrite_source | 4.226x  [rewrite: Add k-dimension loop tiling (K_TILE=256) outside the existing k loop in all three matrix multiplies; initialize the output subtiles only on the first k-tile iteration and then accumulate. Also reduce TILE_SIZE from 64 to 32 to decrease the E/F/G subtile footprint and increase cache residency of B/D/F rows. Keep the ikj order unchanged.]content empty, falling back to reasoning_con |
| 4 | rewrite_source | 4.510x  [rewrite: In each matrix multiply (E:=A*B, F:=C*D, G:=E*F), replace the direct update of the output sub-tile with a local scalar accumulator array of size TILE_SIZE for the j dimension. For every i in the i-tile: 1) declare a local array 'DATA_TYPE sum[TILE_SIZE]' initialized to zero; 2) inside the k loop, accumulate aik * B[k][j] into sum[j]; 3) after the k loop, write sum[j] to E[i][j].  |
| 5 | rewrite_source | 5.481x  [rewrite: 对三个矩阵乘(E:=A*B, F:=C*D, G:=E*F)，在每个(i_tile,j_tile)内对k循环新增K_TILE=256的分块；每个k块开始时，将该k块范围及当前j_tile对应的B/D/F子矩阵(连续行)复制到栈上的自动二维数组(如B_local[256][TILE_SIZE])；最内层循环改用该局部数组进行计算，保持原ikj顺序。TILE_SIZE保持64不变，E/F/G仍直接更新。F和G部分同理处理。] |
| 6 | rewrite_source | 4.964x  [rewrite: 对三个矩阵乘（E:=A*B, F:=C*D, G:=E*F）的最内层j循环（即每次迭代中累加 aik*B[k][j] 的循环）添加 #pragma clang loop vectorize(enable) vectorize_width(8) 和 #pragma clang loop interleave(enable) interleave_count(4)，保持原有的i/j tiling结构、ikj顺序以及所有计算不变。目标：强制编译器使用512位向量宽度和更高交织因子，提升吞吐量并隐藏延迟。] |
| 7 | rewrite_source | 5.869x  [rewrite: 对三个矩阵乘法（E:=A*B, F:=C*D, G:=E*F）的最内层 j 循环（ikj 顺序）进行手动展开因子为 8 的寄存器分块：对于每个 i，将 j 循环按 8 展开，声明 8 个局部累加器（DATA_TYPE acc0..acc7 初始化为 0），在 k 循环内将 aik 与 B[k][j+0..7] 相乘并累加到相应累加器；k 循环结束后将累加器值写回 E[i][j+0..7]。剩余 j 元素（不足 8 的尾部）使用原始累加方式处理。保持已有 i/j tiling 结构不变，不改变浮点运算顺序，数值结果与 baseline 一致。] |
| 8 | rewrite_source | 5.223x  [rewrite: 将三个矩阵乘法（E:=A*B, F:=C*D, G:=E*F）的 TILE_SIZE 从 64 修改为 128，保持 i/j 双层 tiling 结构和 ikj 循环顺序不变，不引入任何额外局部数组或累加器，仅改变分块大小以更好利用 L2 缓存并降低 tile 循环开销。] |
| 9 | rewrite_source | 5.220x  [rewrite: 在 current_best 的 i/j tiling + ikj 结构上，对三个矩阵乘法（E:=A*B, F:=C*D, G:=E*F）的最内层 j 循环进行手动展开，步长 16，声明 16 个局部累加器（DATA_TYPE acc0..acc15 初始化为 0），在 k 循环内将 aik 与 B[k][j+0..15] 分别乘加进对应累加器，k 循环结束后将 16 个累加器值写回 E[i][j+0..15]；尾部不足 16 的元素用标量累加。保持原有 tile 大小、i/j 分块和循环顺序不变。] |

</details>

<details><summary><b>adi</b> — 中位加速比 <b>3.5547x</b>（基线 6598.28 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb025`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：4.1627x　最终确认：**3.5547x**　IQR [3.5416, 3.7707]　base_cv=0.1% best_cv=3.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.881x  [rewrite: 对 Column Sweep 和 Row Sweep 的 i 循环分别进行 cache blocking：将 i 循环按块大小 B（如 128）划分为外层 ii 循环和内层 i 循环，保持 j 递推方向不变。Column Sweep 块内多个 i 可共享 u 的相邻列；Row Sweep 块内多个 i 可共享 v 的相邻行，从而改善时间局部性。] |
| 2 | rewrite_source | 0.971x  [rewrite: 在 Column Sweep 中，对 i 循环按块大小 32 分块，每个块内先将 u 的所需列（从 i_start-1 到 i_end+1）复制到一个局部连续二维数组 u_block[N][block_sz+2] 中，使 j 循环对 u 的访问变为连续；同样对 Row Sweep 的 v 访问进行打包。同时在 kernel 函数内部通过 restrict 限定词声明局部指针来避免编译器因潜在别名而保守优化。仍然保持 j 递推顺序不变。] |
| 3 | rewrite_source | 1.534x  [rewrite: Apply loop interchange to both Column Sweep and Row Sweep: exchange the i (outer) and j (inner) loops so that j becomes the outer loop and i becomes the inner loop. Extract the boundary initializations (v[0][i]=1.0, p[i][0]=0.0, q[i][0]=...) into separate i-only loops before the j loop. After interchange, the inner i loop is fully independent and can be vectorized; u[j][i] reads  |
| 4 | rewrite_source | 1.403x  [rewrite: 在 Column Sweep 的 forward 循环（j 外层 i 内层）中分配局部数组 p_prev[N] 和 q_prev[N] 暂存上一 j 步的 p[i][j] 和 q[i][j] 值，初始化 p_prev[i]=0.0, q_prev[i]=v[0][i]。每次 j 循环计算 denom = a * p_prev[i] + b, invdenom = 1.0 / denom，然后 p[i][j] = -c * invdenom, q[i][j] = (…) * invdenom，最后更新 p_prev[i] = p[i][j], q_prev[i] = q[i][j]。这样消除对全局数组 p[i][j-1], q[i][j-1] 的 stride‑N 读取，并将除法从两次减为一次。] |
| 5 | rewrite_source | 0.993x  [rewrite: Undo loop interchange only for Row Sweep: revert the row sweep loops (both forward and backward passes) to the original i-outer, j-inner order, while keeping the column sweep loops in their interchanged j-outer, i-inner form. Retain all boundary initialisation extractions (separate i-loops for v[0][i], p[i][0], q[i][0] etc.) exactly as they are in the current best source.] |
| 6 | rewrite_source | 1.715x  [rewrite: Insert #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) before every i‑inner loop in both column sweep and row sweep (forward elimination and backward substitution loops), keeping all existing loop interchange and boundary extraction intact. This forces 256‑bit SIMD vectorization and loop interleaving, converting the innermost independent iterations into |
| 7 | rewrite_source | 2.697x  [rewrite: 在 Column Sweep 的 forward 循环中引入局部数组 prev_p[N] 和 prev_q[N] 缓存上一行的 PCOL 和 QCOL 值：初始化 prev_p[i]=PCOL[0][i]、prev_q[i]=QCOL[0][i]，在 j 循环内计算当前行时使用 prev_p[i] 和 prev_q[i] 替代跨步读取 PCOL[j-1][i] 和 QCOL[j-1][i]，计算完成后立即更新 prev_p[i]=PCOL[j][i]、prev_q[i]=QCOL[j][i]。保留现有的循环交换与所有 #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) 标注，不改变其他循环结构。] |
| 8 | rewrite_source | 4.163x  [rewrite: 在 Row Sweep 的 forward/backward 循环中，分配临时缓冲区 vbuf[4][_PB_N]，将 unroll-and-jam 因子增加至 4。对于 i 步长为 4 的主循环，在 j 循环前拷贝 v[i-1], v[i], v[i+1], v[i+2], v[i+3] 行到 vbuf 的各行，然后将 forward/backward 内对 v 的直接访问替换为 vbuf 下标连续访问。对于剩余 i 尾循环，使用同样的缓冲区策略，但只拷贝所需的行数。所有 buffer 在函数入口用 malloc 分配，出口 free。保留现有的向量化 pragma、循环交换和 Column Sweep 缓存不变。] |
| 9 | rewrite_source | 3.382x  [rewrite: In the Row Sweep forward loop (both unroll-and-jam factor‑4 block and the tail row loop), replace each occurrence of `p[i][j] = -f / (d*p[i][j-1]+e)` followed by `q[i][j] = (...)/(d*p[i][j-1]+e)` with a scalar denom = d*p[i][j-1]+e, invdenom = 1.0/denom, then p[i][j] = -f * invdenom and q[i][j] = (...) * invdenom. Do this for all four unrolled i and the tail i, keeping the exact  |

</details>

<details><summary><b>atax</b> — 中位加速比 <b>0.9826x</b>（基线 13.87 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c1_pb012`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.2443x　最终确认：**0.9826x**　IQR [0.8850, 1.0689]　base_cv=17.9% best_cv=14.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.913x  [rewrite: Loop tiling on outer i-loop with tile size B=64: split for(i=0;i<M;i++) into for(ii=0;ii<M;ii+=B) and for(i=ii;i<min(ii+B,M);i++), keeping inner j-loops unchanged. This reuses y vector across i iterations in cache, reducing memory traffic on y.] |
| 2 | rewrite_source | 0.905x  [rewrite: Two-level tiling on the y-update loop: block the i-loop with tile size Ti=32 and the j-loop with tile size Tj=256, so that a sub-block of y stays in cache while the inner i-tile processes the corresponding A sub-block. Compute the entire tmp vector first (i outer, j inner), then tile the y-update as: for ii, for jj, for i in ii tile, for j in jj tile: y[j] += A[i][j] * tmp[i]. Ke |
| 3 | rewrite_source | 0.639x  [rewrite: Loop interchange on y-update loop: swap loops to make j outer, i inner, and use a local scalar to accumulate y[j] before storing back. Specifically, change 'for(i) for(j) y[j] += A[i][j]*tmp[i];' to 'for(j) { DATA_TYPE acc = y[j]; for(i) acc += A[i][j]*tmp[i]; y[j] = acc; }'. Keep tmp computation unchanged.] |
| 4 | rewrite_source | 1.072x  [rewrite: Manually unroll the innermost y-update loop (for(j=0; j<_PB_N; j++) y[j] += A[i][j]*tmp[i]) by a factor of 4, replicating the body four times with j offsets j, j+1, j+2, j+3, and adjusting the loop bound and remainder handling. Keep the outer i-loop and all other code identical, preserving scalar tmp and the original intratenation ordering.] |
| 5 | rewrite_source | 0.976x  [rewrite: Manually unroll the tmp computation loop (for(j=0; j<_PB_N; j++) tmp[i] = tmp[i] + A[i][j] * x[j]) by a factor of 4, replicating the body with j offsets j, j+1, j+2, j+3, adjusting the loop bound and adding a remainder loop for leftover iterations. Keep the already unrolled y-update loop and all other code unchanged.] |
| 6 | rewrite_source | 1.014x  [rewrite: Unroll-and-jam the outer i-loop by a factor of 2: process two consecutive i values together. Use two scalar accumulators tmp0 and tmp1 for tmp[i] and tmp[i+1]. Compute them simultaneously in a single j-loop: tmp0 += A[i][j]*x[j]; tmp1 += A[i+1][j]*x[j]; then in the subsequent y-update loop (already unrolled 4x), update each y[j+k] with A[i][j+k]*tmp0 + A[i+1][j+k]*tmp1. Adjust th |
| 7 | rewrite_source | 0.919x  [rewrite: Manually unroll the tmp-computation inner loop (for(j=0; j<_PB_N; j++) tmp[i] = tmp[i] + A[i][j]*x[j]) by a factor of 4, using four separate scalar accumulators (sum0, sum1, sum2, sum3) that each accumulate over strided indices (j, j+1, j+2, j+3), then after the loop combine them as tmp[i] = sum0 + sum1 + sum2 + sum3. Keep the already 4-way unrolled y‑update loop unchanged to pre |
| 8 | rewrite_source | 0.839x  [rewrite: Further unroll the already-4‑way‑unrolled y‑update loop to 8‑way: inside the outer i‑loop, replace the `for(j=0; j<=_PB_N-4; j+=4)` block and its remainder with `for(j=0; j<=_PB_N-8; j+=8)` containing eight statements `y[j+k] = y[j+k] + A[i][j+k] * tmp[i]` for k=0..7, and adjust the trailing remainder loop accordingly. Keep all other code (tmp computation, initialization) unchang |
| 9 | rewrite_source | 1.244x  [rewrite: In the y‑update portion (already 4‑way unrolled), hoist `tmp[i]` into a local `DATA_TYPE tmp_val = tmp[i];` just before the unrolled j‑loop, then replace every occurrence of `tmp[i]` in the loop body with `tmp_val`. Leave the tmp‑computation loop and the remainder handling unchanged.] |

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>1.0000x</b>（基线 14.18 ms，hash 校验，0/0 次为正，⚠ 正确性门无效）</summary>

- 任务 `c1_cb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0678x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.001x  [rewrite(utils/qsortx): Inline the swap and shorts (short-sort) functions directly into qsortx and replace the goto-based recursion with a while-loop that uses the explicit stack (lostk/histk) for partition processing, eliminating function call overhead and improving branch predictability.] |
| 2 | rewrite_source | 1.068x  [rewrite(utils/qsortx): 1) Introduce median-of-three pivot selection (first, mid, last) in the partition step to improve balance and reduce worst-case comparisons. 2) In the insertion sort branch (size <= CUTOFF), replace the SWAP-based sifting with a shift-and-insert: copy the current element to a VLA buffer ([width]), use memmove to shift the sorted prefix right by one element, then memc |
| 3 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] optimized run timed out |
| 4 | rewrite_source | 0.996x  [rewrite(utils/qsortx): Increase the insertion sort cutoff from the current CUTOFF macro to a constant 64 and enlarge the on-stack temporary buffer from 256 to 1024 bytes, so that more subarrays are handled by the optimized shift-and-insert insertion sort instead of the partition loop.] |
| 5 | rewrite_source | 0.980x  [rewrite(utils/qsortx): 将 SWAP 宏重写为：当 width <= 256 时使用栈上 char[256] 临时缓冲区，通过 memcpy 实现两次拷贝完成交换；否则回退到原有逐字节循环。这避免分区和 pivot 选择中的逐字节交换，减少内存写入次数和循环开销。] |
| 6 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp60fxk02p/rw_utils_utils_shadow/polybench.c:665:25: error: use of undeclared identifier 'uint32_t' |
| 7 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmppo4qzxld/rw_utils_utils_shadow/polybench.c:661:25: error: use of undeclared identifier 'uint32_t' |
| 8 | rewrite_source | 1.026x  [rewrite(utils/qsortx): 在分区循环的两个 do-while 条件表达式中，使用 __builtin_expect 提示 comp 比较结果为真（即元素小于/大于等于 pivot）以提高分支预测准确率：将 comp(loguy, lo) <= 0 替换为 __builtin_expect(comp(loguy, lo) <= 0, 1)，将 comp(higuy, lo) >= 0 替换为 __builtin_expect(comp(higuy, lo) >= 0, 1)。] |
| 9 | rewrite_source | 1.003x  [rewrite(utils/qsortx): 在插入排序的 shift-and-insert 代码段中，当元素宽度 width <= sizeof(long) (8字节)时，使用内联的逐字节 while 循环实现 memmove 语义（char* 逐字节搬移），并将当前元素保存于栈上的 long 变量（通过逐字节 memcpy 拷贝），完全避免调用库函数 memmove/memcpy；对于 width > 8 的元素保持原有 memmove/memcpy 路径不变。该变换针对小元素高频插入排序场景，消除函数调用开销和参数准备，保留原有 median-of-three 及分区逻辑。] |

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>1.0000x</b>（基线 4.76 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb002`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpzj0m3ovt/automotive_susan_corners_rewrite.c:1454:1: error: unknown type name 'We' |
| 2 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp1q3j3orz/automotive_susan_corners_rewrite.c:1633:3: error: type specifier missing, defaults to 'int'; ISO C99 and later do not support implicit int [-Wimplicit-int] |
| 3 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpxfm4o8ko/automotive_susan_corners_rewrite.c:1768:1: error: function definition is not allowed here |
| 4 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp6xyawoyc/automotive_susan_corners_rewrite.c:1739:1: error: function definition is not allowed here |
| 5 | rewrite_source | 0.922x  [rewrite: 在 susan_corners 函数中，对每个像素计算 USAN 面积的最内层循环（掩膜邻域遍历），将直接累加到输出数组元素（如 out[i][j] += ...）的操作，改为使用局部 int 变量 area 累加，循环结束后将 area 写入输出数组对应位置。不修改任何循环边界、分支条件或计算表达式，新增的局部变量需显式 int 类型，确保不产生嵌套函数定义或隐式类型错误。] |
| 6 | rewrite_source | 0.961x  [rewrite: 在 susan_corners 函数遍历图像行的外层循环内，对后续即将访问的行数据使用 __builtin_prefetch 进行软件预取。针对每个输出像素在内层掩膜邻域遍历中会访问周边多行输入数据的特点，在 i 循环开头预取 in[i+2] 行的起始地址（若未越界），并设置 locality hint 为中等（3）以保持在 L2/L3 缓存中。只添加预取语句，不修改任何计算逻辑、循环边界或输出格式。] |
| 7 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpoegdbry7/automotive_susan_corners_rewrite.c:1454:1: error: unknown type name 'We' |
| 8 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpjhc2oiaa/automotive_susan_corners_rewrite.c:1455:44: error: restrict requires a pointer or reference ('CORNER_LIST' (aka 'struct (unnamed at /home/hanning/comet/tmp/tmpjhc2oiaa/automotive_susan_corners_rewrite.c:311 |
| 9 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpwi71h014/automotive_susan_corners_rewrite.c:1740:1: error: function definition is not allowed here |

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>1.0478x</b>（基线 8.42 ms，hash 校验，41/51 次为正）</summary>

- 任务 `c1_cb003`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0679x　最终确认：**1.0478x**　IQR [1.0146, 1.0660]　base_cv=4.0% best_cv=4.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.008x  [rewrite: Apply loop tiling (cache blocking) to the hottest nested loops — likely those iterating over 2D pixel arrays in `susan_thin`. Since source is missing, the generic transformation would tile the inner two loops (e.g., over rows and columns) with block sizes tuned to fit L1 cache (approx 1.3 MiB for 20 instances), aiming to reuse data in cache and reduce memory traffic. Exact loop v |
| 2 | rewrite_source | 1.028x  [rewrite: 在 susan_thin 的最内层循环中，为每个中心像素 (i,j) 预先加载其 3x3 邻域（必要时扩展到 5x5）的 mid 和 r 值到局部数组 mid3x3[3][3] 和 r3x3[3][3] 中，然后所有后续的条件分支、计数、比较均使用这些局部变量，仅在需要修改 mid 数组时写回。此举消除对相邻像素的重复二维地址计算和冗余内存读取，同时不改变任何数据依赖或跳转逻辑。] |
| 3 | rewrite_source | 1.048x  [rewrite: 在 susan_thin 的 n==1 分支中，将 scale 数组的乘法循环展开为 9 条显式乘法语句，并将 3×3 最大值搜索的嵌套循环替换为 9 次 if 比较序列，直接更新 a,b,m。此举消除小循环的索引计算和跳转开销，完全保留原始计算语义。] |
| 4 | rewrite_source | 1.045x  [rewrite: 在 susan_thin 最外层循环内，紧接 mid 3x3 邻域预加载后，立即计算 8 个相邻像素 (m00,m01,m02,m10,m12,m20,m21,m22) 的 mid <8 布尔值并存入局部标量变量（如 is00,is01,...,is22）。用这些变量累加得到 n；在 n==1 分支中直接用它们移位构建 mask；在 n==2 和 n>2 分支中替换所有 mXX<8 条件判断，消除重复的比较运算。] |
| 5 | rewrite_source | 1.068x  [rewrite: 在 susan_thin 的参数声明中为 int* r 和 uchar* mid 加入 restrict 关键字，消除两个数组间的别名歧义；同时将内层依据 n 的多路 if-else 链重写为 switch(n) 语句（保持原有的 n==1 内 *m_center<6 筛选逻辑），利用跳转表减少分支指令及预测失误。] |
| 6 | rewrite_source | 1.031x  [rewrite: 在 susan_thin 内层循环中，预加载 3x3 邻域 mid 值后立即构造一个 8-bit 邻居掩码（m00<8 置 bit0，m01<8 置 bit1，...），用 __builtin_popcount(mask) 得到 n。之后 switch(n) 所有分支中用位测试（mask & (1<<k)）取代原有的 mXX<8 比较，完全消除重复比较指令；同时将 n==2 中基于 b00/b02/b20/b22 的 x,y 选择改为以掩码低 4 位索引的查表（lut_x[16], lut_y[16]），并将后继条件转换为位运算，减少分支。] |
| 7 | rewrite_source | 1.059x  [rewrite: 在susan_thin现有最优源码（含restrict和switch）基础上，将n==2分支内为查找邻居r值而声明的局部数组neighbour_vals[9]及其初始化移除，改用基于(y,x)的三元运算符直接从预加载的标量r00..r22中选择r_neighbour；同时将后续条件中多次出现的行偏移表达式（如(2*y)*stride、(2*x)等）提前计算为局部常量指针或整型偏移，避免重复乘法，最终减少栈分配和地址计算指令。] |
| 8 | rewrite_source | 1.060x  [rewrite: 在 n==1 分支中将局部数组 l[9] 替换为 9 个标量变量（l0..l8），修改 scale 乘法和最大值搜索使用这些标量；同时在外层 if(*m_center<8) 前使用 __builtin_expect(..., 0) 标记该条件不常见，帮助编译器将内层代码移出主路径。] |
| 9 | rewrite_source | 1.038x  [rewrite: 基于 current_best 源码（已包含 restrict 和 switch 的 susan_thin），在最外层循环内、`if (*m_center < 8)` 之前插入 `if (__builtin_expect(*m_center < 8, 0))`，代替原条件判断。提示编译器该条件极少成立（大部分像素值≥8），使其将条件成立时的全部代码（包括整个 switch 及长分支）位移到函数末尾作为冷路径，而将主执行流作为 fall‑through，改善取指效率和分支预测准确度。不改变任何数据逻辑。] |

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.1674x</b>（基线 61.80 ms，hash 校验，9/9 次为正）</summary>

- 任务 `c1_cb004`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1700x　最终确认：**1.1674x**　IQR [1.1670, 1.1691]　base_cv=0.2% best_cv=0.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): output hash mismatch (ref=0e44d5392b3e, opt=400ab85810c4) |
| 2 | rewrite_source | 失败 [rewrite_source] optimized version returned non-zero exit code -11 |
| 3 | rewrite_source | 0.997x  [rewrite: 对susan_smoothing中的图像遍历双层循环应用cache tiling，将行和列均以32为块大小分块，外层循环按块索引，内层循环在块内执行与原始完全相同的平滑计算（逐像素、逐邻域窗口累加），不引入任何标量累加器或改变运算顺序，以确保数值结果完全一致。] |
| 4 | rewrite_source | 0.996x  [rewrite: 手动展开 susan_smoothing 中最内层的邻域权重求和循环（窗口大小编译时常量），显式列出所有偏移和权重，同时完整保留原始的边缘越界条件判断，使用标量累加器按原始顺序累加，消除动态循环索引开销并维持数值等价。] |
| 5 | rewrite_source | 1.092x  [rewrite: 在 susan_smoothing 函数中，将输入/输出图像指针声明为 `float* __restrict__`，并在函数入口使用 `__builtin_assume_aligned(in, 64)` 和 `__builtin_assume_aligned(out, 64)` 告知编译器首地址对齐；同时将内层窗口循环中的权重表（若为编译期常量）用 `const float` 局部数组显式定义并标记 `__attribute__((aligned(64)))`。保持所有累加顺序与原始完全一致，不引入任何标量累加器重排。] |
| 6 | rewrite_source | 1.164x  [rewrite: 在 susan_smoothing 的 large Gaussian 分支中，对窗口循环的外层 for(y=-mask_size; y<=mask_size; y++) 进行 strip‑mine，步长设为 2：每次迭代处理两个相邻 y 行的完整 x 循环，先累加第一个 y 行的所有 x，再累加第二个 y 行的所有 x，尾部单独处理剩余单个 y 行。保持 dpt 和 ip 的访问顺序与原代码完全一致，不改变浮点累加次序。] |
| 7 | rewrite_source | 1.167x  [rewrite: Apply cache tiling to the outer image loops (i, j) in the large Gaussian branch. Use a tile size of 32x32: introduce two outer loops for i_tile and j_tile stepping by 32, and compute smoothing for each pixel inside the tile using the existing window computation with the y-strip-mined inner loops. Ensure that scalar accumulators (area, total) are per-pixel local and that the summa |
| 8 | rewrite_source | 1.170x  [rewrite: 将 `bp_local` 查找表复制从每个像素循环内移出到 tile 循环外部，在 `i_tile` 循环开始前一次性复制 `bp` 到栈上的局部数组（`bp_local`），所有 tile 内像素共享该表，消除重复的 256 字节复制开销。保持其他结构（tiling、y‑strip‑mining、标量累加顺序）完全不变。] |
| 9 | rewrite_source | 1.162x  [rewrite: 在 susan_smoothing 的大高斯分支中，将 y 循环的 strip-mining 步长从 2 增加到 4：每次迭代处理四个连续的 y 行（y 递增 4），先依次处理四个 y 行的所有 x 像素（保持原有的 dpt 和 ip 访问顺序），然后统一处理尾部不足 4 的单个行。保持所有累加顺序与原始一致，不引入任何数值改变。] |

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>1.6024x</b>（基线 23.77 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb013`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.6545x　最终确认：**1.6024x**　IQR [1.5951, 2.0693]　base_cv=18.8% best_cv=3.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.567x  [rewrite: 使用标量累加器替代对 q[i] 的重复加载/存储，并将 r[i] 提升到内层循环外以减少寄存器压力；考虑对内层循环手动展开4次以隐藏FP延迟。] |
| 2 | rewrite_source | 1.611x  [rewrite: 对外层 i 循环和内层 j 循环进行 2D 分块（tiling）。将迭代空间沿 i 和 j 两个维度划分成大小为 B_i×B_j 的小块，使得每个小块内的 s 数据能够装入 L1 缓存。具体地，将原始的双层循环重组为四重循环：外层按 i 分块，次外层按 j 分块，然后在分块内执行原始的 i 和 j 迭代，保持原始的计算顺序（对每个 i 计算 q_acc 并更新 s[j]）。分块大小宜为 64×64 或 128×128，以确保 s 的一块可以反复使用。] |
| 3 | rewrite_source | 0.977x  [rewrite: 交换分块内层循环的嵌套顺序：将原始代码中的 'for (ii = i0; ii < i_end; ii++) { for (jj = j0; jj < j_end; jj++) { ... } }' 改为 'for (jj = j0; jj < j_end; jj++) { for (ii = i0; ii < i_end; ii++) { ... } }'，并将 r[ii] 保持在 ii 循环内。这样 s[jj] 在外层 jj 循环中固定，可被提升为标量累加，减少对 s 的重复读-修改-写操作；同时 q_tmp[ii-i0] 的累加在内层 ii 循环中为连续访问，仍可向量化。保持所有数组索引不变以确保数值一致性。] |
| 4 | rewrite_source | 1.617x  [rewrite: 在 ii 循环内部，将 jj 循环展开成 4 路标量累加器：引入局部数组 s_acc[4]，初始化 0；对 jj 循环以步长 4 展开，每次迭代计算 4 个 jj 位置的 s 累加值，使用 s_acc[0..3] 分别累加 r[ii]*A[ii][jj+0..3]，同时 q_tmp 也分别累加；循环结束后将 s_acc[0..3] 加回 s[jj..jj+3]。这样将 4 次独立的 s 更新合并，减少了 s 数组的重复读-修改-写操作，并允许编译器更好地向量化和乱序执行。] |
| 5 | rewrite_source | 1.565x  [rewrite: 在 ii 循环内部，将 jj 循环的展开步长从 4 增加到 8：引入 s_acc[8] 并初始化为 0，jj 循环每次处理 8 个连续元素，分别累加 ri*A[ii][jj+0..7] 到 s_acc[0..7]；q_tmp 仍按 jj 递增顺序累加（保持浮点加法顺序）。循环结束后将 s_acc[0..7] 写回 s。这合并了更多 s 更新，减少读-修改-写次数，增加指令级并行。] |
| 6 | rewrite_source | 1.576x  [rewrite: 在 j0 分块内部，引入一个长度为 (j_end - j0) 的局部数组 s_local（使用栈分配或 scratch buffer），并初始化为 0。在 ii 循环中，将所有对 s[jj] 的累加改为对 s_local[jj - j0] 的累加，完全不读取 s；ii 循环结束后，再将 s_local 的每个元素加到 s[j0:j_end] 上。保持 q_tmp 原有的 4 路展开不变，同时保留原有的 2D 分块参数 Bi=64, Bj=256。这样消除了 s 的读-修改-写依赖，减少了全局 s 的缓存冲突和内存写入次数。] |
| 7 | rewrite_source | 1.655x  [rewrite: 调整 2D 分块参数：将 Bi 从 64 增加到 128，Bj 从 256 减小到 128，使 s 和 q_tmp 的缓存占用总和保持在约 2.5 KiB 以内，同时增加 ii 维度的重用，期望提升 A 矩阵的 L1 命中率和浮点流水线利用率。保持内层 jj 循环的 4 路标量累加不变。] |
| 8 | rewrite_source | 1.285x  [rewrite: 在 j0 分块内，将 p[j0:j_end-1] 复制到栈上局部数组 p_block（长度 Bj），在 ii 循环内用 p_block 替换 p 的访问，保持原有 4 路展开和 Bi=128, Bj=128 分块参数不变。] |
| 9 | rewrite_source | 1.641x  [rewrite: 调整分块参数：将 Bi 从 128 增加到 256，Bj 从 128 减小到 64，使 q_tmp 大小 2KB 和 s 大小 512B 总和仍在 L1 内，同时增加 ii 维度的复用距离，减少 A 和 p 的缓存缺失。保持内层 jj 循环的 4 路标量累加不变。] |

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>0.9822x</b>（基线 51.69 ms，hash 校验，4/11 次为正）</summary>

- 任务 `c1_cb005`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1171x　最终确认：**0.9822x**　IQR [0.9560, 1.0191]　base_cv=3.2% best_cv=3.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.066x  [rewrite: 将三个独立的 argList 遍历循环（统计文件名和最长文件名、解析短标志、解析长标志）合并为一个循环，在同一个遍历中同时完成文件计数、最长文件名更新、短标志解析和长标志解析，以减少链表遍历次数和分支开销。] |
| 2 | rewrite_source | 1.035x  [rewrite: 将统计文件名/最长文件名的第一个循环与已合并标志解析的第二个循环合并为单一循环，在遍历 argList 同一个节点时，先处理标志（若未遇到“--”），然后如果不以'-'开头，则更新文件计数和最长文件名，从而将两次链表遍历减少为一次。] |
| 3 | rewrite_source | 0.989x  [rewrite: 将 progName 中查找最后一个 PATH_SEP 的手动 for 循环替换为 strrchr 调用，利用优化的库实现；并将 strstr 调用结果预存到局部变量以避免重复调用，减少控制流开销。] |
| 4 | rewrite_source | 0.945x  [rewrite: 在已合并的大循环内，将短标志解析的 switch 语句改为 if-else 链，按常见程度排列 '-c', '-d', '-f', '-k', '-v' 等标志，数字 case '-1'..'-9' 用范围判断处理；同时将末尾 while 释放循环中的 'if (aa->name != NULL) free(aa->name)' 简化为 'free(aa->name)'，利用 free(NULL) 的安全语义消除分支。] |
| 5 | rewrite_source | 0.988x  [rewrite: 在标志解析的 for (aa = argList; aa != NULL; aa = aa->link) 循环体开头，将 aa->name 加载到局部变量 CurName，后续所有对 aa->name 的访问全部替换为该局部变量，以减少编译器重复计算基址。同时，将常用长标志的 ISFLAG 字符串比较结果预取到局部常量标志变量中，例如提前用 strcmp 判断 '--stdout' 等常用选项并设为 bool，后续 if 链直接检查局部 bool 而非反复调用 strcmp。] |
| 6 | rewrite_source | 1.036x  [rewrite: 在标志解析的 for (aa = argList; aa != NULL; aa = aa->link) 循环的开头，将 aa->name 加载到局部 const Char *name，并将循环内所有对 aa->name 的解引用（包括 ISFLAG 宏、aa->name[0]、aa->name[1]、aa->name[j]）替换为 name 或 name[idx]，以减少编译器可能无法消除的重复指针加载。不做任何字符串比较预计算。] |
| 7 | rewrite_source | 0.963x  [rewrite: 在已合并的循环内，将短标志解析的内层 for (j = 1; name[j] != '\0'; j++) 改写为 const Char *p = name + 1; while (*p != '\0') { switch (*p) { ... } p++; }，用指针递增消除索引变量 j 和索引寻址开销，保持 switch 逻辑不变。] |
| 8 | rewrite_source | 1.117x  [rewrite: 在遍历 argList 链表的 for 循环体内，在处理当前节点之前，插入 __builtin_prefetch(aa->link, 0, 3) 以预取下一个链表节点的地址，隐藏指针追踪的访存延迟。] |
| 9 | rewrite_source | 1.023x  [rewrite: 在argList遍历循环前，将srcMode, opMode, forceOverwrite, keepInputFiles, smallMode, noisy, verbosity, blockSize100k, numFileNames, longestFileName等全局变量加载到同名局部变量；循环体内所有对这些变量的读写均使用局部变量，循环结束后将可能被修改的局部变量逐个写回对应全局变量。同时，在循环体开头将aa->name赋给局部const Char *name，并将所有aa->name的解引用替换为name，进一步减少指针加载。保留已有的__builtin_prefetch(aa->link, 0, 3)。] |

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>1.0139x</b>（基线 93.92 ms，hash 校验，5/7 次为正）</summary>

- 任务 `c1_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0785x　最终确认：**1.0139x**　IQR [0.9716, 1.0280]　base_cv=2.7% best_cv=1.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.994x  [rewrite: 合并短标志和长标志的两个 for 循环为一个，对每个节点一次处理，避免重复遍历 argList 链表。] |
| 2 | rewrite_source | 0.961x  [rewrite: Fuse the three sequential for-loops that traverse argList (filename length/count, short-flag parsing, long-flag parsing) into a single loop. Introduce a boolean like `stopFlags` initially false; when '--' is encountered, set it to true. Inside the single loop, always compute filename length/count; conditionally parse short flags and long flags only when `stopFlags` is false. This |
| 3 | rewrite_source | 1.041x  [rewrite: 在三个独立遍历 argList 的 for 循环中（文件名统计、短标志解析、长标志解析），每个循环内，在处理当前节点 aa 的末尾（或进入循环体后立即预取下一节点），插入 `if (aa->link) __builtin_prefetch(aa->link, 0, 3);`，以预取下一个链表节点到 L1 缓存，避免指针追逐时的缓存缺失延迟。] |
| 4 | rewrite_source | 1.079x  [rewrite: In the three argList-traversing for-loops, annotate the branch conditions that distinguish plain filenames from flags (i.e., ISFLAG("--") checks and aa->name[0] == '-') with __builtin_expect(expr, 0) to mark the unlikely path. Keep the existing __builtin_prefetch for the next node. This should reduce branch-mispredict stalls on the common filename path.] |
| 5 | rewrite_source | 1.013x  [rewrite: 将所有 ISFLAG 宏调用替换为手动内联的字符比较，避免 strcmp 外部函数调用开销。对于 "--" 检查，使用 `(aa->name[0] == '-' && aa->name[1] == '-' && aa->name[2] == '\0')`；对于其他标志如 "--stdout" 等，逐字符比较并检查终止符，确保语义不变。保留已有的 __builtin_expect 和 __builtin_prefetch。] |
| 6 | rewrite_source | 1.010x  [rewrite: Allocate a contiguous array of Cell* from the linked list argList (after totalArgs is known). Populate it in one traversal, then replace all subsequent linked-list traversals (three loops) with index-based array traversals. This eliminates pointer chasing and enables hardware prefetching, reducing cache misses.] |
| 7 | rewrite_source | 0.965x  [rewrite: Allocate a contiguous array of Cell* from argList (size totalArgs). Populate it in one traversal (retaining __builtin_prefetch for the list traversal itself), then replace all three subsequent for-loops over argList with index-based loops over this array. Keep the existing __builtin_expect annotations on branch conditions inside those loops. This combines the branch-hint speedup  |
| 8 | rewrite_source | 0.940x  [rewrite: In the three downstream operand loops (OM_Z, OM_UNZ, and test), annotate the ISFLAG("--") check and the condition aa->name[0] == '-' with __builtin_expect(expr, 0) to mark them as unlikely paths, similar to what was done in the earlier parsing loops. Keep all existing optimizations.] |
| 9 | rewrite_source | 0.990x  [rewrite: Lift frequently‑used global variables (srcMode, opMode, blockSize100k, smallMode, forceOverwrite, keepInputFiles, noisy, verbosity, workFactor, numFileNames, longestFileName, numFilesProcessed, progName, etc.) into local variables at the top of kernel_bzip2_encode, operate on the locals throughout the three parsing loops, and write back the final values to the globals only right  |

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0947x</b>（基线 5778.15 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb016`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.1521x　最终确认：**1.0947x**　IQR [1.0946, 1.0960]　base_cv=0.1% best_cv=0.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.011x  [rewrite: Apply 2D tiling (cache blocking) to the j and k loops inside the main i loop. Choose a tile size (e.g., 64) and restructure the loops into outer j-block and k-block loops, so that A[i][k] stays in cache while multiple j values are updated. The division by A[j][j] is performed immediately after the inner k-blocks finish for a given j, preserving the original numerical order.] |
| 2 | rewrite_source | 1.007x  [rewrite: Introduce a local scalar accumulator inside the k-loop for A[i][j]. Load A[i][j] into a local variable 'sum_j', perform sum_j -= A[i][k]*A[j][k] over the whole k range, then store sum_j back into A[i][j] once. Apply the same transformation to the A[i][i] update: load into 'sum_i', accumulate sum_i -= A[i][k]*A[i][k], store back, and then apply sqrt, keeping the original arithmeti |
| 3 | rewrite_source | 0.996x  [rewrite: Fuse the j-loop (off-diagonal update + division) with the subsequent k-loop (diagonal update) by moving the A[i][i] accumulation into the j-loop right after A[i][j] /= A[j][j]. This eliminates the separate k-loop and avoids loading A[i][k] a second time, reducing memory traffic.] |
| 4 | rewrite_source | 0.979x  [rewrite: Refactor the kernel to use explicit flat 1D indexing: declare a local 'double* restrict A_flat = (double*)A;' then replace every A[row][col] with A_flat[row*N+col] throughout the loop body. This removes potential pointer aliasing and allows the compiler to vectorize the inner k-loop reduction. Also change the outer tile block size from 64 to 32, because 64*64*8=32 KB per tile may |
| 5 | rewrite_source | 0.972x  [rewrite: Inside the existing 2D tiled loops (j and k), add scalar accumulators: for each j in the current j-tile, accumulate the inner product over k-blocks into a local 'sum_j', then divide by A[j][j] and store back to A[i][j] once per j. For the diagonal, keep a 'sum_i' accumulator across all j-blocks for the current i, updating A[i][i] only after all j-blocks finish, preserving arithme |
| 6 | rewrite_source | 1.003x  [rewrite: For each i in the tiled loops, create a local restrict pointer Ai = &A[i][0]; for each j, create Aj = &A[j][0]; then replace all A[i][k] by Ai[k] and A[j][k] by Aj[k] in the inner k-loop and the diagonal update. Use Ai[j] for updates to A[i][j]. This provides strict aliasing information (restrict) and pointer-based access with stride-1, enabling the compiler to auto-vectorize the |
| 7 | rewrite_source | 1.049x  [rewrite: Implement a blocked Cholesky factorization with block size B=64. For each diagonal block jb, factor it using the standard small in-place Cholesky. For each block row ib > jb, solve the triangular system to compute the off-diagonal block. Then update the trailing submatrix by subtracting A(ib,jb) * A(kb,jb)^T for kb>jb, using local temporary matrices to increase cache locality and |
| 8 | rewrite_source | 1.053x  [rewrite: In the trailing update step (part 3), for each (kb,ib) pair introduce two 64×64 local double arrays (aligned to 64 bytes) – tempL_ib and tempL_kb – copy the current block columns A[ib+ii][jb+jj] and A[kb+kk][jb+jj] into them in row‑major order, then perform the symmetric update A[ib+ii][kb+kk] -= sum_{jj} tempL_ib[ii][jj] * tempL_kb[kk][jj] using a nested loop with the jj reducti |
| 9 | rewrite_source | 1.152x  [rewrite: In the trailing update (part 3), for each (kb, ib) pair, after creating tempL_ib and tempL_kb_T, allocate a third local buffer `double (*temp_update)[bk] = aligned_alloc(64, bi*bk*sizeof(double))` initialized to zero. Perform the triple loop over jj, ii, kk accumulating into temp_update[ii][kk]. After the triple loop, copy temp_update back to A[ib+ii][kb+kk] with a simple nested  |

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>1.0000x</b>（基线 1.54 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb007`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1102x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.035x  [rewrite(utils/TIFFWriteScanline): Insert __builtin_expect (likely/unlikely) hints on performance-critical branches: WRITECHECKSTRIPS and BUFFERCHECK failures as unlikely, row >= td->td_imagelength as unlikely, strip != tif->tif_curstrip as unlikely (common case: strip unchanged), row < tif->tif_row as unlikely (common case: forward sequential write). Also mark the final tif->tif_encoderow |
| 2 | rewrite_source | 1.007x  [rewrite(utils/TIFFWriteScanline): At the beginning of TIFFWriteScanline, after loading 'td', cache td->td_imagelength and td->td_nstrips into local const variables 'imagelength' and 'nstrips'. Replace all subsequent uses of td->td_imagelength and td->td_nstrips in the hot path and rare path with these locals. Also cache tif->tif_scanlinesize into a local variable before the hot-path call. |
| 3 | rewrite_source | 0.614x  [rewrite(utils/TIFFWriteScanline): Convert TIFFWriteScanline to a static inline function (with __attribute__((always_inline))) so that it is fully inlined into the calling loop in kernel_consumer_tiff2bw. This eliminates function call overhead and allows the compiler to optimize across iterations, potentially unrolling or pipelining the indirect call to encoderow.]content empty, falling ba |
| 4 | rewrite_source | 0.949x  [rewrite(utils/TIFFWriteScanline): Refactor TIFFWriteScanline to extract all code after the early hot‑path return into a separate static __attribute__((cold)) function (e.g., TIFFWriteScanlineCold). The hot path itself then consists only of the safety checks, the early‑return condition (with existing __builtin_expect hints), and a tail call/return to the cold function when the early return |
| 5 | rewrite_source | 1.110x  [rewrite(utils/TIFFWriteScanline): Ensure WRITECHECKSTRIPS and BUFFERCHECK are inlined: if they are currently macros, wrap them in static inline functions with __attribute__((always_inline)) and __builtin_expect to force inlining and correct branch prediction, reducing call overhead on the hot path without changing semantics.] |
| 6 | rewrite_source | 0.997x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 中引入一个 tif_flags 位 (例如 TIFF_FASTSEQ) 缓存 'sequential write within same strip' 状态。一旦慢路径确认当前 strip、行列号均合法且编码器已设置，便置位此标志。热路径仅检查该标志位、row == tif->tif_row 和 tif->tif_encoderow 是否就绪，若通过则直接用缓存在局部变量的编码器函数指针调用并递增行号，完全跳过 td->td_imagelength、td->td_nstrips、strip 比较等所有结构体字段访问。慢路径负责清除标志位并重新验证状态。此变换保持所有外部行为不变，仅缩短最频繁的执行路径。] |
| 7 | rewrite_source | 0.620x  [rewrite(utils/TIFFWriteScanline): Wrap the entire slow-path code (the block after the early hot-path return) in `if (__builtin_expect(0, 0)) { ... }`. This instructs the compiler to treat the contained code as cold and place it at the end of the function, reducing instruction-cache pressure on the hot path without introducing any function-call overhead.] |
| 8 | rewrite_source | 0.982x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 函数中，将整个热路径条件 (tif->tif_flags & TIFF_CODERSETUP) && strip == tif->tif_curstrip && row == tif->tif_row && row < td->td_imagelength && strip < td->td_nstrips 使用 __builtin_expect(..., 1) 包裹，以明确提示编译器该路径为极常执行。同时在条件之前将 tif->tif_encoderow 和 tif->tif_scanlinesize 分别加载到局部变量 encode_row 和 scanline_sz，并在热路径调用中使用这些局部变量，避免从内存/结构体再次加载。不改变任何逻辑，不拆分函数，不标 |
| 9 | rewrite_source | 1.009x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 入口处，将 tif->tif_curstrip 和 tif->tif_row 分别加载到 const 局部变量并用于热路径条件（strip == curstrip_local、row == row_local），但不替换慢路径中对这些字段的修改或后续引用。同时保留之前对 td->td_imagelength/td->td_nstrips 的局部缓存（若已存在）。不引入任何新的 likely/unlikely 标记，不预加载 encoderow 或 scanlinesize，仅减少热路径中的 struct 成员间接访问次数。] |

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>1.0000x</b>（基线 2.41 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb008`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1845x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.929x  [rewrite(utils/TIFFWriteDirectory): 将 for (b = 0; b <= FIELD_LAST; b++) 循环中的字段计数改为使用 __builtin_popcount 对 td->td_fieldsset 数组的每个元素进行快速计数，并手动调整 FIELD_SUBFILETYPE 加倍逻辑；同时将 TIFF_SWAB 分支内的字节交换 for 循环手动展开 4 次（使用宏或手动写重复体）以减少迭代开销。] |
| 2 | rewrite_source | 0.715x  [rewrite(utils/TIFFWriteDirectory): 在 TIFFWriteDirectory 函数中，当 (tif->tif_flags & TIFF_SWAB) 条件成立时，在字节交换循环 `for (dir = (TIFFDirEntry*) data; dircount; dir++, dircount--)` 之前插入 `#pragma clang loop vectorize(enable) vectorize_width(4)`，指示编译器尝试以 128 位（4 个 32 位元素）向量宽度进行字节交换，利用 SIMD 指令缩短循环；在不改变数组语义的前提下，此变换仅影响编译器决策，保持原始字节顺序输出。同时可考虑在循环内将 dir++ 与 dircount-- 合并为单次指针更新以简化 bound 检查，但非必须。] |
| 3 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmprftg7kei/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 4 | rewrite_source | 0.812x  [rewrite(utils/TIFFWriteDirectory): 将三次独立的 WriteOK 调用（分别写 dircount、目录数据、diroff）合并为一次：在 SWAB 处理之后，分配一个临时缓冲区，将 dircount、data（dirsize 字节）和 diroff 按顺序拷贝进去，然后调用一次 WriteOK 写出整个缓冲区并释放，替换原有的三行 WriteOK 及中间的 TIFFSeekFile 调用（Seek 仍保留在写入前）。] |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpba2r5rst/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 6 | rewrite_source | 0.966x  [rewrite(utils/TIFFWriteDirectory): 在 TIFFWriteDirectory 函数中，对多个条件分支添加 __builtin_expect 提示：将 `tif->tif_mode == O_RDONLY` 标记为 unlikely(0)；`tif->tif_flags & TIFF_POSTENCODE` 标记为 unlikely(0)；`tif->tif_rawcc > 0 && !TIFFFlushData1(tif)` 标记为 unlikely(0)；`tif->tif_diroff == 0` 标记为 likely(1)；`tif->tif_dataoff & 1` 标记为 unlikely(0)；`tif->tif_flags & TIFF_SWAB` 标记为 unlikely(0)。不改变任何数据逻辑或控制流。] |
| 7 | rewrite_source | 0.931x  [rewrite(utils/TIFFWriteDirectory): 在 TIFFWriteDirectory 中，将 `_TIFFmemcpy(fields, td->td_fieldsset, sizeof (fields));` 替换为显式的逐元素赋值（例如 for 循环），以消除 memcpy 可能带来的编译器别名障碍，允许后续 FieldSet 宏重复读取 fields 时被提升到寄存器；同时，将 TIFF_SWAB 分支之后的 `TIFFSwabShort(&dircount);` 和 `TIFFSwabLong(&diroff);` 替换为等价的纯 C 位操作字节交换（避免函数调用开销，编译器可将其优化为 bswap 指令）。其余代码不变。] |
| 8 | rewrite_source | 1.184x  [rewrite(utils/TIFFWriteDirectory): 将字段计数循环 for (b = 0; b <= FIELD_LAST; b++) 替换为遍历 td->td_fieldsset 数组（共 FIELD_SETLONGS 个元素），对每个元素调用 __builtin_popcountl 累加，同时根据 FIELD_SUBFILETYPE 范围正确添加额外的 +1 计数，消除逐位测试的循环开销。注意正确处理 FIELD_SUBFILETYPE 的加倍逻辑，不改变其他代码。] |
| 9 | rewrite_source | 0.982x  [rewrite(utils/TIFFWriteDirectory): Simplify the bit mask generation in the field counting popcount loop: remove the temporary variable '_max_bit', compute '_keep' directly from the condition, and fold the mask construction to reduce register pressure and eliminate a dead store. No other changes.] |

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>1.0000x</b>（基线 0.94 ms，hash 校验，0/0 次为正，⚠ 正确性门无效）</summary>

- 任务 `c1_cb009`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3297x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpy5kqrkhv/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 2 | rewrite_source | 1.054x  [rewrite(utils/TIFFReadDirectory): 将第二个for循环中查找 tif_fieldinfo 的线性扫描（最坏 O(n^2)）替换为二分查找：对每个 dp->tdir_tag，使用二分查找在已排序的 tif->tif_fieldinfo 数组中定位该 tag，同时保留乱序警告逻辑（用上次 tag 值比较，若下降则发一次警告）。并缓存 dp->tdir_tag 到局部变量减少重复解引用。] |
| 3 | rewrite_source | 1.056x  [rewrite(utils/TIFFReadDirectory): 在第二个 for 循环（for (dp=dir, n=dircount; n>0; n--, dp++)）开始处，将 tif->tif_fieldinfo 和 tif->tif_nfields 提升为局部变量 TIFFFieldInfo** local_finfo = tif->tif_fieldinfo; int local_nf = tif->tif_nfields;，并修改循环内所有对这两个成员的引用（二分查找条件、field_bit 检查、fip 赋值等）使用局部变量，以消除重复的 tif 结构体间接访问。]content empty, falling back to reasoning_content on attempt 1 |
| 4 | rewrite_source | 0.610x  [rewrite(utils/TIFFReadDirectory): 在第三个 for 循环（for (dp=dir, n=dircount; n>0; n--, dp++)）开始处，将 tif->tif_dir.td_nstrips、tif->tif_dir.td_samplesperpixel、tif->tif_dir.td_planarconfig 等只读字段提升为局部变量，并替换循环内所有对它们的直接引用。同时使用 __builtin_expect 标注 TIFFError、MissingRequired 等错误路径和未知标签警告为 unlikely，以减少分支预测误差。] |
| 5 | rewrite_source | 1.084x  [rewrite(utils/TIFFReadDirectory): 在第一个 for 循环完成字节序交换后、第二个 for 循环开始前，分配局部数组 uint16 tags[dircount] 并将 dir[i].tdir_tag 依次复制到 tags[i]；然后将第二个和第三个 for 循环中所有对 dp->tdir_tag 的访问（包括缓存后的局部变量 tag）替换为对 tags[n-1] 的读取，以消除对 TIFFDirEntry 结构体中非必要字段的缓存加载，提升数据局部性。] |
| 6 | rewrite_source | 1.330x  [rewrite(utils/TIFFReadDirectory): 在第一个 for 循环中，同时提取 dir[i].tdir_type 和 dir[i].tdir_count 到局部数组 uint16 types[dircount] 和 uint16 counts[dircount]（已有 tags 数组）；然后在第二个和第三个 for 循环中，将所有对 dp->tdir_type 和 dp->tdir_count 的访问替换为 types[n-1] 和 counts[n-1]，保留 dp 指针仅用于传递给需要 TIFFDirEntry* 的函数（如 TIFFFetchNormalTag）。] |
| 7 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpuw7ha6v2/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 8 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp0j7q7rgq/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 9 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmps0qw3l3i/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>1.0000x</b>（基线 3.11 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb010`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0360x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.970x  [rewrite(utils/TIFFWriteDirectory): 将第一个循环（计算 nfields）分裂为两个顺序循环：第一个遍历 b = 0 到 FIELD_SUBFILETYPE-1，每个有效字段贡献 2；第二个遍历 b = FIELD_SUBFILETYPE 到 FIELD_LAST，每个有效字段贡献 1。这样完全消除循环体内部的条件表达式 (b < FIELD_SUBFILETYPE ? 2 : 1)。] |
| 2 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] optimized version returned non-zero exit code -6 |
| 3 | rewrite_source | 1.036x  [rewrite(utils/TIFFWriteDirectory): Combine the three WriteOK calls that write the directory (dircount, data block, diroff) into a single WriteOK call by constructing a contiguous buffer containing the uint16 dircount, the dirsize bytes of directory entries, and the uint32 diroff, then writing everything at once. This reduces system call and function-call overhead.] |
| 4 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpji2a_vym/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp4ug997cz/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 6 | rewrite_source | 0.990x  [rewrite(utils/TIFFWriteDirectory): 在计算 nfields 的循环（for (b = 0; b <= FIELD_LAST; b++)）中，用一个静态预计算的局部数组 `static const char field_weight[] = { ... }` 存储每个 b 的贡献值（SUBFILETYPE 以下的值为 2，其余为 1），将循环体改为 `nfields += field_weight[b];`，从而消除条件表达式 "(b < FIELD_SUBFILETYPE ? 2 : 1)" 带来的分支。] |
| 7 | rewrite_source | 1.020x  [rewrite(utils/TIFFWriteDirectory): 在计算 nfields 的循环 (for (b = 0; b <= FIELD_LAST; b++)) 中，将条件 `if (TIFFFieldSet(tif, b))` 替换为直接测试位域：`if (td->td_fieldsset[b>>3] & (1 << (b & 7)))`，其中 td = &tif->tif_dir。这消除了每次迭代对库函数 TIFFFieldSet 的调用开销，不改变语义。] |
| 8 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpwpm6g5fx/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 9 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] optimized version returned non-zero exit code -11 |

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>16.3119x</b>（基线 4894.24 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c1_pb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：16.2598x　最终确认：**16.3119x**　IQR [16.1871, 16.4292]　base_cv=0.7% best_cv=0.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 13.433x  [rewrite: 将计算相关矩阵的三重循环从原来的i(外层)-j(中层)-k(内层)顺序重排为k(外层)-j(中层)-i(内层)顺序，让最内层i循环沿data[k][i]的行方向连续访问；同时预先初始化整个corr矩阵为0并对角线置1，在所有k迭代完成后对称复制下三角（或即时对称赋值）。] |
| 2 | rewrite_source | 16.077x  [rewrite: Perform tiling on the k loop (block size BK e.g. 64) to keep data rows in cache, and optionally tile the i loop (block size BI e.g. 32) to keep corr updates within a cache-resident submatrix. The tiled structure will be: for (kk=0; kk<N; kk+=BK) for (i=0; i<M-1; i+=BI) for (k=kk; k<min(kk+BK,N); ++k) for (i_in=i; i_in<min(i+BI,M-1); ++i_in) { DATA_TYPE aik = data[k][i_in]; for ( |
| 3 | rewrite_source | 14.290x  [rewrite: Implement k-loop tiling with block size BK=64 in the correlation accumulation region. Within each k-tile, for each i-row compute the partial sum of aik * data[k][j] into a local scalar accumulator array (e.g., DATA_TYPE sum[128]) initialized to zero before the j-loop, then accumulate into sum[j] instead of directly into corr[i][j]. After the inner j-loop, add sum[j] to corr[i][j |
| 4 | rewrite_source | 15.796x  [rewrite: 在已交换为 k‑i‑j 的相关积累区域，增加 k 维度显式分块 (KK 块大小 BK=128) 和 i 维度分块 (II 块大小 BI=64)。结构为 for (kk) for (ii) for (k in kk block) for (i in ii block) for (j = i+1 to M-1) corr[i][j] += data[k][i] * data[k][j]。j 最内层保持连续访问 corr 行，k 分块减少 data 行跨块重读，i 分块确保 corr 更新集留在 L1 cache。不使用临时累加器数组，直接更新 corr，数值一致。] |
| 5 | rewrite_source | 11.575x  [rewrite: Transform the correlation accumulation into a blocked matrix multiplication C = A^T * A. Specifically: for kk from 0 to N step BK, for ii from 0 to M step BI, for jj from ii to M step BJ, use a local 2D accumulator array C_block[BI][BJ] initialized to zero. For each k in the k-block, load data[k][ii:ii+BI] and data[k][jj:jj+BJ] into local vectors, then compute the outer product  |
| 6 | rewrite_source | 11.659x  [rewrite: Introduce a j-dimension register blocking inside the k/i-tiled loops: for each i in the i-tile, iterate j in blocks of size 8 (starting from max(i+1, jj_block_start)), initialize 8 scalar accumulators to zero, loop over k to accumulate aik * data[k][j+offset], and then add the accumulators to corr[i][j..j+7]. Use BK=64 and BI=16 to keep working set in L1 and reduce register pres |
| 7 | rewrite_source | 15.696x  [rewrite: Restructure the correlation accumulation region to: for (ii=0; ii<M; ii+=32) for (kk=0; kk<N; kk+=64) for (jj=ii; jj<M; jj+=32) for (k=kk; k<min(kk+64,N); ++k) for (i=ii; i<min(ii+32,M-1); ++i) { DATA_TYPE aik = data[k][i]; for (j=max(i+1,jj); j<min(jj+32,M); ++j) corr[i][j] += aik * data[k][j]; }. The k‑tile (BK=64) sits outside jj so that the same short data window is reused a |
| 8 | rewrite_source | 16.260x  [rewrite: In the correlation accumulation region, increase the k tiling block size BK to 512 (or even 1024 if data size permits) while keeping the i tiling block size BI at 32. Use loop order: for (kk=0; kk<N; kk+=BK) for (i=0; i<M-1; i+=BI) for (k=kk; k<min(kk+BK,N); ++k) for (i_in=i; i_in<min(i+BI,M-1); ++i_in) { DATA_TYPE aik = data[k][i_in]; for (j=i_in+1; j<M; ++j) corr[i_in][j] += a |
| 9 | rewrite_source | 16.162x  [rewrite: 将 k 分块大小 BK 从 512 增加到 1024，以进一步减少外部 k 循环迭代次数，提高 data 行在 L2/L3 缓存中的重用；保持 i 分块大小 BI=32 不变；其他循环结构不变。注意边界处理使 k 的上限正确。] |

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>13.4514x</b>（基线 1447.97 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb002`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：14.7627x　最终确认：**13.4514x**　IQR [12.6233, 14.3160]　base_cv=2.1% best_cv=8.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 9.669x  [rewrite: 将计算协方差的三重循环（i, j, k）中的外层 i 循环与内层 k 循环交换，使 k 成为最外层循环、i 成为中层循环、j 保持内层。这样 data[k][i] 和 data[k][j] 的访问变为同行的连续 stride-1 访问，显著改善向量化和缓存局部性。注意 cov[i][j] 累加顺序仍为 k 递增，数值与原代码一致。] |
| 2 | rewrite_source | 8.418x  [rewrite: 对三重循环 for(k=0; k<N; k++) for(i=0; i<M; i++) for(j=i; j<M; j++) 应用 k 和 i 两维度的 loop tiling：外部分块循环遍历 k 块和 i 块，块大小为 32 或 64，内部用标量累加器或矢量化访问更新 cov[i][j]，使得 data 块和 cov 块在 L1 缓存中保持复用，减少 capacity miss。数值累加顺序不变（k 递增），保证与原始结果一致。] |
| 3 | rewrite_source | 13.104x  [rewrite: 对当前三重循环（k, i, j）实施三维 cache blocking：将 i 和 j 维度按块大小 BI=BJ=64 划分，k 维按 BK=256 划分。用六层嵌套循环：外层遍历 k 块，中层遍历 i 块和 j 块（满足 jj >= ii 以保持三角约束），内层在块内执行原始 k、i、j 计算，更新 cov[i][j] += d_ki * data[k][j]。此变换将数据重用限制在小块内，显著减少主存流量，且不改变浮点累加顺序。]content empty, falling back to reasoning_content on attempt 1 |
| 4 | rewrite_source | 14.763x  [rewrite: 将 cov 矩阵的 i/j 分块循环提升到 k 分块循环之外（即 ii,jj 最外层），每个 cov 块内声明一个栈上临时局部数组（大小如 BI×BJ=64×64）作为累加器，初始化为 0.0。然后内层遍历所有 k 块（kk）及其 k 循环，将 data[k][i] 与 data[k][j] 的乘积累加到局部数组对应位置。完成所有 k 迭代后，一次性将局部数组中的值赋给 cov[i][j]（或 += 因为 cov 已初始化为 0）。这样将整个 k 维度的归约限制在快速局部存储中，消除对 cov 元素的重复 load/store。] |
| 5 | rewrite_source | 14.426x  [rewrite: 在当前ii/jj块循环内部栈上声明局部累加数组 float temp[BI][BJ] 并初始化为0.0；将原有的kk级k循环移入ii/jj块循环内部，在内层k循环中累积 d_ki * data[k][j] 到 temp[i][j]；完成所有k迭代后，将temp一次性加回cov[i][j]（cov已初始化为0）。维持原分块参数BI=BJ=64、BK=256，若编译器提示寄存器压力可适当调小BI/BJ。] |
| 6 | rewrite_source | 14.139x  [rewrite: 将 ii 和 jj 分块循环提升到 kk 分块循环的最外层，即 for (ii) for (jj) { ... for (kk) { ... } }。在 ii,jj 块内部声明 float temp[32][32] 并初始化为 0.0，然后内层遍历所有 kk 块和 k 循环，将 data[k][i] * data[k][j] 累加到 temp[i][j]（按 i,j 相对块内索引），最后一次性将 temp 的值赋给 cov[ii+i][jj+j]（cov 已初始化为 0）。为配合 L1 缓存，将分块参数调整为 BI=32, BJ=32, BK=128。] |
| 7 | rewrite_source | 13.356x  [rewrite: 在 ii/jj 外层遍历的每个 BI×BJ（64×64）temp 块内部，增加 4×4 的寄存器分块（register tiling）：声明 float acc[4][4] 初始化为 0.0；在 k 循环中，对 i 块和 j 块以 4 为步长遍历，将 data[k][i]*data[k][j] 累加到 acc 对应元素；每个 4×4 子块完成累加后，将 acc 写入 temp 对应位置。最后如常将 temp 写回 cov。] |
| 8 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): 第二层验证失败: [STANDARD_DATASET] Numeric mismatch: max relative error 2.04e-04 at index 3744 (ref=49.04, opt=49.03), epsilon=2.00e-04 |
| 9 | rewrite_source | 14.469x  [rewrite: 首先动态分配 float dataT[_PB_M][_PB_N] 并执行转置：将data[k][j]复制到dataT[j][k]，使得所有后续计算中dataT的行连续访问。然后基于dataT计算协方差矩阵（对称矩阵乘法C = A * A^T的下三角部分）：采用ii,jj分块遍历cov下三角，每个块内声明栈上局部临时数组temp[BI][BJ]作为累加器；在k循环中顺序访问dataT[i][k]和dataT[j][k]（均为连续stride-1），将乘积累加入temp；完成所有k后一次性将temp写回cov。最后释放dataT。分块尺寸沿用当前最优的BI=BJ=64，BK=256，并保留原有的三角约束和#pragma omp simd。此变换将所有data访问变为连续stride-1，最大化向量化效率与缓存带宽，预期显著超越14.763x。] |

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>1.9502x</b>（基线 133.68 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb022`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.9757x　最终确认：**1.9502x**　IQR [1.9085, 2.0006]　base_cv=5.6% best_cv=3.9%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.352x  [rewrite: Swap the loop order of the two vertical filter loops (forward y1 and backward y2) so that the outer loop is over i (rows) and the inner loop is over j (columns). This changes memory access from column-major to row-major, improving cache locality. The dependence chain (tm1/tp1, ym1/yp1, ym2/yp2) remains along i, which is now the outer loop, so correctness is preserved.] |
| 2 | rewrite_source | 1.603x  [rewrite: Fuse the second horizontal filter (backward scan over j) with the first merge loop: compute y2 locally and directly accumulate into imgOut using y1, avoiding storing y2. Similarly, fuse the second vertical filter (backward scan over i) with the final merge loop: compute y2 locally and write the final imgOut = y1 + y2, eliminating the standalone merge loop and the corresponding y2 |
| 3 | rewrite_source | 1.588x  [rewrite: Apply cache tiling to the j-dimension of the vertical forward (y1 computation) and vertical backward (final imgOut) loops. Split the inner j-loop into blocks of size 64 (tunable). Insert #pragma clang loop vectorize(enable) and #pragma clang loop interleave(enable) before the tiled inner loops to force SIMD vectorization. This improves L1 reuse of the state vectors and enables SI |
| 4 | rewrite_source | 1.976x  [rewrite: 对 kernel 中水平向前循环（第一个 for(i) for(j) 计算 y1）和水平向后+合并循环（第二个 for(i) for(j) 计算 imgOut）做 i-j 循环交换，并将 j 维度分割为 tile 块（块大小 64）。外层为 j 的分块循环，中间为 i 的全遍历循环，最内层为 j 在块内的遍历。这样在内层 i 循环上可对多行数据同时进行 SIMD 向量化（依赖链在 j 方向不变，各行独立）；j 分块保持对 imgIn/y1/imgOut 的行优先访问局部性。对交换后的内层 i 循环添加 #pragma clang loop vectorize(enable) vectorize_width(4) 启用 256-bit 向量化。垂直循环维持现有结构。] |
| 5 | rewrite_source | 1.942x  [rewrite: 对垂直向前循环（for (i=0; i<_PB_W; i++) 内层 for (j=0; j<_PB_H; j++)）和垂直向后循环（for (i=_PB_W-1; i>=0; i--) 内层 for (j=0; j<_PB_H; j++)）的内层 j 循环插入 #pragma clang loop vectorize(enable) vectorize_width(4) 以启用 256-bit SIMD 向量化。j 方向各行独立，无循环携带依赖，加速逐元素乘加运算。保持现有水平循环交换与分块不变。] |
| 6 | rewrite_source | 1.835x  [rewrite: Modify the j-tile size of the interchanged horizontal forward loop (first y1 computation) and the horizontal backward/merge loop (imgOut computation) from 64 to 128. Keep the i-j loop interchange structure: outer j block over height, middle over all i, inner over j in block, with #pragma clang loop vectorize(enable) vectorize_width(4) on the inner i loop. This reduces block loop  |
| 7 | rewrite_source | 1.181x  [rewrite: 对垂直向前循环（for (i=0; i<_PB_W; i++) { for (j=0; j<_PB_H; j++) ... }）和垂直向后循环（for (i=_PB_W-1; i>=0; i--) { for (j=0; j<_PB_H; j++) ... }）引入 j 维度的 cache tiling，块大小 64。具体为：添加外层 j_block 循环，中间保持原 i 循环顺序，内层遍历块内 j。在内层 j 循环前插入 #pragma clang loop vectorize(enable) vectorize_width(4) 以启用 256-bit SIMD。此变换将状态数组 tm1_vec/ym1_vec/ym2_vec 等的工作集限制在每个块内，提高 L1 命中率，并利用向量化加速垂直乘加运算。] |
| 8 | rewrite_source | 1.500x  [rewrite: 从 current_best 中移除水平向前循环（第一个 for(i) 块中的内层 v 循环）和水平向后+合并循环（第二个 for(i) 块中的内层 v 循环）前的所有 #pragma clang loop vectorize(enable) vectorize_width(4) 指令，让编译器自行决定最优的向量化宽度和循环展开。保持当前循环交换和分块结构不变。] |
| 9 | rewrite_source | 1.342x  [rewrite: 将水平向前循环和水平向后/合并循环中的 j 分块大小从 64 改为 32。保持 i-j 循环交换结构和 #pragma clang loop vectorize(enable) vectorize_width(4) 不变。] |

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>3.9812x</b>（基线 242.92 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb014`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：3.9394x　最终确认：**3.9812x**　IQR [3.9671, 4.0545]　base_cv=0.5% best_cv=0.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 3.929x  [rewrite: 对 r 和 q 循环进行二维分块（tiling），引入块大小 B_R 和 B_Q。将内层 s-p 矩阵乘法重组为 tile 形式：对于每个 tile (rr, qq)，计算 A[rr:rr+B_R][qq:qq+B_Q][:] 与 C4 的乘积，利用临时累加器或直接更新 A 块。目标是将 C4 保留在 L1 缓存中重复使用，减少 A 的回读。保持原始计算语义：每个 (r,q) 的 sum 向量计算一致，只是调整了遍历顺序。] |
| 2 | rewrite_source | 3.907x  [rewrite: 在现有的二维 r/q 分块（tiling）基础上，对分块内计算的核心循环添加 pragma：对 p 循环添加 #pragma clang loop vectorize(enable) 强制向量化，对 s 循环添加 #pragma clang loop interleave(enable) 以增加指令级并行；同时将分块大小常数化并设置为 B_R=32, B_Q=32，确保每个 tile 的数据集（C4 行 × A 子块）能完全驻留在 L1 缓存。保留原始语义，所有变换仅通过 pragma 和常量调整实现。] |
| 3 | rewrite_source | 3.939x  [rewrite: 在已有的 r/q 二维分块基础上，对 s 维度与 p 维度同时进行分块（二维 tile），将 C4 划分为 B_S × B_P 的小块。对于每一个 r/q 分块，引入一个本地累加数组 buf[NP] 或直接使用寄存器变量，遍历 s 块，对当前 s 块内每一行，计算对 p 块的贡献，累加到局部累加器；完成一个 s-p 块后，将局部累加器的值累加至全局 sum，最终写回 A。保持与原代码相同的浮点累加顺序（s 顺序不变，p 累加顺序不变），选择 B_S、B_P 使 C4 小块可装入 L1 或寄存器（如 32×64）。] |
| 4 | rewrite_source | 3.786x  [rewrite: 在现有的 s/p 二维分块结构基础上，对最内层 p 循环使用标量累加器：在每个 r,q 迭代中，对每个 p 引入一个寄存器变量 acc，初始化为 sum[p]，在 s 分块内累加 a_val * C4[s][p]，s 分块结束后将 acc 写回 sum[p]。同时将分块尺寸设为 B_S=16, B_P=128，以增加 C4 块在 L1 中的重用并适配 SIMD 宽度。保持原始浮点累加顺序不变。] |
| 5 | rewrite_source | 3.898x  [rewrite: 在现有 r/q 二维分块（B_R=64,B_Q=64）基础上，对内层 s 和 p 循环同时应用 4×4 微内核优化：将 s 循环以 4 为步长、p 循环以 4 为步长展开，使用完全展开的乘累加（即对 s 的 4 个连续行、p 的 4 个连续列计算 a_val * C4[s][p] 并累加到 sum[p]），保持原始 s 顺序递增、p 顺序递增的累加语义。进一步将 s 分块大小设为 B_S=32 以平衡循环开销和缓存重用。不改变 r/q 分块结构，不引入额外临时数组。] |
| 6 | rewrite_source | 3.799x  [rewrite: 基于当前 r/q 和 s/p 二维分块结构，对 s 分块内层 p 循环使用标量累加器优化：引入一个长度为 B_P 的局部寄存器累加器数组 (例如 double acc[B_P])，初始化后在整个 s 块内累加 a_val * C4[s][p] 到 acc[p]，s 块结束后将 acc 累加到全局 sum 再写回 A。设置分块大小 B_S=64, B_P=64 以平衡缓存重用和寄存器压力，保持 s 循环原始递增顺序以保证浮点累加顺序不变。] |
| 7 | rewrite_source | 2.386x  [rewrite: 基于当前最优 r/q 分块（B_R=64,B_Q=64）和 s/p 二维分块结构，将 s 和 p 分块大小均缩小为 B_S=8、B_P=8；在每一 s 分块内，使用寄存器数组 acc[8] 累加 A[r][q][s] * C4[s][p] 的结果，s 分块结束后将 acc 累加至 sum 再写回 A；在最内层 p 块循环前插入 #pragma clang loop vectorize(enable) vectorize_width(4) 以强制 256 位 SIMD 向量化，并保持原始 s→p 累加顺序不变。] |
| 8 | rewrite_source | 3.895x  [rewrite: 在 kernel_doitgen 函数的三个数组参数 A、C4、sum 前添加 __restrict 关键字（即 POLYBENCH_3D(A,...) 等前插入 restrict 限定符），以提示编译器这些数组互不重叠，消除别名分析障碍，允许更激进的向量化、循环展开和指令重排。不改变任何循环结构、分块尺寸或累加顺序。] |
| 9 | rewrite_source | 3.785x  [rewrite: 将全局 sum 数组参数替换为每个 (r,q) 迭代内的栈上局部数组 sum_local[NP]：在 r/q 循环体内声明 double sum_local[_PB_NP] 并初始化为 0，后续所有对 sum 的累加与赋值均改用 sum_local，最后将 sum_local 内容写回 A。移除或忽略传入的 sum 指针。保持现有 r/q 分块（B_R=64, B_Q=64）和 s/p 二维分块结构及原始累加顺序不变。该变换强制 sum 地址不逃逸，允许编译器将 sum_local 提升为向量寄存器，消除 sum 的内存读写和别名冲突。] |

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.0083x</b>（基线 2.29 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c1_pb017`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.5346x　最终确认：**1.0083x**　IQR [0.4382, 1.8647]　base_cv=39.7% best_cv=32.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.535x  [rewrite: 消除临时数组z：对每个k，将原来的z[i]=y[i]+alpha*y[k-i-1]与拷贝循环替换为单遍循环，迭代i从0到(k-1)/2，同时用标量保存y[i]和y[k-i-1]旧值以计算新值并写回，处理对称对；若k为偶数则单独处理中心元素；从而省去整个z数组及其拷贝循环。] |
| 2 | rewrite_source | 1.023x  [rewrite: 添加restrict限定的指针声明：在函数开始时创建DATA_TYPE *__restrict yp = y; DATA_TYPE *__restrict rp = r; 并在反转循环处创建DATA_TYPE *__restrict r_rev_p = r_rev; 将后续所有访问y、r、r_rev的操作改用这些restrict指针，显式打破别名关系，帮助编译器对点积循环和更新循环进行向量化。] |
| 3 | rewrite_source | 0.940x  [rewrite: 在点积循环处插入 #pragma clang loop vectorize(enable) vectorize_width(4) 和 #pragma clang loop unroll_count(4)；在 in-place 更新循环处插入 #pragma clang loop vectorize(enable) vectorize_width(4)，强制编译器生成256位SIMD指令并展开点积循环以增加指令级并行。] |
| 4 | rewrite_source | 0.909x  [rewrite: 在点积循环处，先用局部 restrict 指针指向 r_rev + N - k 和 y，并用 __builtin_assume_aligned 声明 64 字节对齐，然后在该循环前添加 #pragma clang loop vectorize(enable)（不指定宽度），让编译器自行选择最佳 SIMD 宽度并对齐加载。同步保持更新循环不变，避免上次因强制向量化带来的退化。] |
| 5 | rewrite_source | 0.952x  [rewrite: 消除r_rev数组：删除反转循环，点积循环中将r_rev[N-k+i]替换为r[k-1-i]，直接逆序读取原始r数组。该变换省去整个r_rev数组的写操作和后续O(N²)次读操作，降低内存带宽压力，且因数据仍在缓存中，逆序访问不会引入显著延迟。] |
| 6 | rewrite_source | 0.910x  [rewrite: 重构 in-place 更新循环，消除 if (i == j) 分支：将循环拆分为两个部分，第一部分处理 i 从 0 到 half（但不包括当 i==j 的中心点），使用对称更新；第二部分单独处理中心点当 k 为奇数时。这样消除循环内条件分支，有助于编译器向量化对称更新。] |
| 7 | rewrite_source | 0.980x  [rewrite: 手动展开点积循环2倍：将for (i=0; i<k; i++) sum += r_rev[N-k+i]*y[i]; 改为 for (i=0; i<k-1; i+=2) { sum += r_rev[N-k+i]*y[i] + r_rev[N-k+i+1]*y[i+1]; } 保留尾部元素处理；保持加法顺序与原始严格一致（两次乘法先做，然后与sum相加，即等同于原始连续累加），以打破单累加器依赖链，提升FMA单元利用率。] |
| 8 | rewrite_source | 1.380x  [rewrite: 在点积循环内部，用局部 const restrict 指针 rr=&r_rev[N-k] 和 yy=y，替代原始的数组索引访问，作用域仅限该循环；更新循环及其他部分不变。目的是打破该循环内 r_rev 与 y 的别名关系，帮助编译器生成更高效的无别名向量化 load，同时避免步骤2中全局 restrict 引入的副作用。] |
| 9 | rewrite_source | 0.898x  [rewrite: 在点积循环内部，声明局部const restrict指针rr = &r_rev[N - k]和yy = y，并用sum += rr[i] * yy[i];替代原来的r_rev[N - k + i] * y[i];，其余代码不变。通过局部restrict打破该循环内r_rev与y的别名关系，帮助编译器生成无别名向量化load，提升点积循环性能。] |

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>1.0206x</b>（基线 873.24 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c1_pb026`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.5688x　最终确认：**1.0206x**　IQR [0.8531, 1.2003]　base_cv=15.6% best_cv=11.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.817x  [rewrite: Apply 2D cache blocking (tiling) to all three spatial update loops inside the time loop. Choose block size 32 for both i and j dimensions. Restructure each loop nest as: loop over i-blocks, loop over j-blocks, then execute the original inner loops over the block’s i/j range. Preserve the original order: first update ey over all blocks, then ex, then hz.] |
| 2 | rewrite_source | 0.843x  [rewrite: Apply 2D cache blocking (tiling) with tile size 16 (instead of 32) for all three spatial update loops: ey, ex, and hz. Add `restrict` qualifiers to the array arguments (ex, ey, hz, _fict_) in the function signature to disambiguate pointer aliasing and enable more aggressive auto-vectorization. Within each tile, preserve the original update order (first ey, then ex, then hz) to ex |
| 3 | rewrite_source | 1.178x  [rewrite: Fuse the ey and ex update loops into a single nested loop to reduce memory traffic for the hz array. For i=1 to NX-1, split the inner loop into j=0 (only ey update) and j=1 to NY-1 (both ey and ex updates) to maintain contiguity and avoid conditionals inside the hot loop. Preserve the original data dependencies and final values exactly.] |
| 4 | rewrite_source | 1.569x  [rewrite: 在 fused 循环（i=1..NX-1, j=1..NY-1）中引入局部标量变量：hz_curr = hz[i][j], hz_up = hz[i-1][j], hz_left = hz[i][j-1]，然后用它们计算 ey 和 ex 更新，消除对 hz[i][j] 的重复加载；在 hz 更新循环中使用标量 prev_ex 缓存 ex[i][j]，每次迭代只 load ex[i][j+1] 即可计算差值并更新 prev_ex；同时在函数签名中对 ex, ey, hz 添加 restrict 限定符。保持数据依赖和数值结果不变。] |
| 5 | rewrite_source | 0.737x  [rewrite: 仅对 hz 更新循环实施 2D cache blocking：引入 i-block 和 j-block 循环（块大小 16），将内层的 i/j 循环限制在块范围内。保持 j 为最内层以保证连续访问。在 hz 更新循环内继续使用 prev_ex 标量缓存（来自 current_best）。ex/ey 更新循环保持不变。] |
| 6 | rewrite_source | 1.139x  [rewrite: 在 current_best 源码的 ex/ey fused 循环（i=1..NX-1 的内层 j 循环）和 hz 更新循环（i=0..NX-2 的内层 j 循环）前分别插入 #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(4)。此外对 hz 更新循环再尝试 #pragma clang loop unroll_count(4)。保持数据依赖不变，只影响编译器 codegen。] |
| 7 | rewrite_source | 1.197x  [rewrite: In the fused ey/ex update loop (i=1..NX-1, j=1..NY-1), introduce a sliding scalar 'prev_hz' initialized to hz[i][0]. Before the inner j loop, load prev_hz = hz[i][0]. Then for j=1, use hz_left = prev_hz, load hz_curr = hz[i][j], compute ey/ex updates, and set prev_hz = hz_curr. This reuses the previous hz_curr as the next hz_left, reducing loads of hz from three per iteration to  |
| 8 | rewrite_source | 1.203x  [rewrite: 在 current_best 源码的三个关键循环前插入 #pragma clang loop vectorize(enable) interleave(enable)：① ey[0][j] 赋值循环；② 主融合 ey/ex 更新循环（i=1..NX-1 内层 j=1..NY-1）；③ hz 更新循环（i=0..NX-2 内层 j=0..NY-2）。不指定 vectorize_width 和 interleave_count，保留编译器自由选择。不改变任何循环结构、标量缓存逻辑和 restrict 限定符。] |
| 9 | rewrite_source | 1.160x  [rewrite: 在 current_best 源码的 hz 更新循环（i 从 0 到 NX-2，j 从 0 到 NY-2）中，i 循环体开头插入 __builtin_prefetch(&hz[i+2][0], 0, 3) 以提前将两行之后的 hz 数据调入缓存，同时保留现有的 prev_ex 标量缓存、restrict 限定符及所有循环结构不变。] |

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>4.0440x</b>（基线 37193.99 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c1_pb023`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：4.5283x　最终确认：**4.0440x**　IQR [3.9243, 4.0736]　base_cv=3.1% best_cv=1.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.545x  [rewrite: Apply cache tiling to the inner two loops over i and j. Introduce tile loops with block size 32 or 64 so that for each k, tiles of path[i][j] are updated while path[i][k] and path[k][j] for that tile are reused from L1/L2 cache. This keeps the k loop outermost to preserve the algorithm's serial dependency.] |
| 2 | rewrite_source | 1.569x  [rewrite: In the existing ii/jj tiled loops, pre‑load path[k][j] for the whole jj tile into a local array pkj_cache[TILE] before entering the i loop. Then inside the i loop reuse that cache instead of reading path[k][j] from the global array. Keep the inner loop order as j inside i to preserve contiguous access to path[i][j].] |
| 3 | rewrite_source | 0.987x  [rewrite: 将常量 TILE 从 64 改为 32，减小每个 tile 的内存占用，使整个 32x32 的双精度 tile（8KB）能稳固驻留在 L1 数据缓存中，同时保留 pkj_cache 预加载机制。] |
| 4 | rewrite_source | 1.499x  [rewrite: 在现有的 ii/jj tiling 基础上，对最外层 k 循环进行额外的分块：将 k 循环分割为外块 kk（块大小 BK，例如 64），将现有 ii 和 jj tile 循环移动到 kk 循环内部、k 内层循环外部，即循环顺序变为 kk -> ii -> jj -> k（内块）-> i -> j。保留 pkj_cache 预加载机制（每次 k 迭代重新加载）。这实现 blocked Floyd-Warshall 算法，使得对于每个 (ii,jj) tile，路径中 path[i][k] 和 path[k][j] 在 k 块内被多个连续 k 迭代复用，从而提升时间局部性并减少内存带宽需求。] |
| 5 | rewrite_source | 1.510x  [rewrite: 在现有的 ii/jj tiling 和 pkj_cache 基础上，对每个 tile 增加列缓存：在 ii 循环开始前将 path[ii..i_end][k] 加载到局部数组 pik_cache；然后将 i 和 j 的内循环进一步 micro‑tile（如 MICRO=16），每个 micro‑tile 先加载 path 对应子块到局部二维数组 micro_path，利用 pik_cache 和 pkj_cache 在 micro_path 中原地完成所有 min 更新（双层 i‑j 循环），最后将 micro_path 写回 path 数组。这消除了内层循环中连续的全局 load‑modify‑store 操作，降低 L1 带宽压力，并让编译器更易向量化局部数组的批量更新。] |
| 6 | rewrite_source | 1.522x  [rewrite: 在现有的 ii/jj tiling 和 pkj_cache 基础上，为每个 ii tile 增加列缓存：在 ii 循环内部、jj 循环之前，将 path[i][k]（i 从 ii 到 i_end）预加载到局部数组 pik_cache[TILE] 中；然后在 i 循环内使用 pik_cache[i-ii] 代替 path[i][k] 加载，以消除多个 jj 块迭代造成的重复 path[i][k] 访问。保留 pkj_cache 预加载以及 i‑j 内循环顺序（j 在内以保持对 path[i][j] 的连续访问）。]content empty, falling back to reasoning_content on attempt 1 |
| 7 | rewrite_source | 4.528x  [rewrite: Implement standard blocked Floyd-Warshall algorithm: block the k loop with block size B=64. Reorder loops as kk (outer blocks of k) → ii (outer blocks of i) → jj (outer blocks of j) → k (inner loop over k block) → i (inner loop over i block) → j (inner loop over j block). In the innermost loop compute path[i][j] = min(path[i][j], path[i][k] + path[k][j]). This keeps B×B tiles of  |
| 8 | rewrite_source | 4.408x  [rewrite: Within each (ii,jj) tile and k block, further tile the i and j loops into 4x4 register blocks. For each 4x4 sub‑tile, load path[i][j] into a local double[4][4] array, perform all min updates using pik and pkj values (scalar or cached), then write back the whole 4x4 block to path. This reduces per‑element L1 store traffic and allows the compiler to keep the sub‑tile in vector regi |
| 9 | rewrite_source | 1.646x  [rewrite: 在现有 blocked Floyd-Warshall 框架中，对每个 (ii,jj) tile 和 k 块，先分配局部二维数组 tile[64][64] 并将 path[ii..i_end][jj..j_end] 复制其中；然后在内层 k 循环中，所有更新都作用于 tile 副本（使用全局的 path[i][k] 和 path[k][j]）；k 块结束后再将 tile 写回 path。对边界不足 64 的 tile 做尺寸自适应。这将消除 tile 自身的重复 load/store，把 O(B×TILE²) 次访存降为 O(TILE²)，预期在已有基础上进一步提升性能。] |

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>1.2940x</b>（基线 172.71 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb003`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.2907x　最终确认：**1.2940x**　IQR [1.2773, 1.2954]　base_cv=3.8% best_cv=3.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.959x  [rewrite: Loop tiling: first globally scale C by beta (for i for j C[i][j] *= beta), then tile i and j with tile sizes 64×64. Restructure loops as for (ii=0; ii<NI; ii+=64) for (jj=0; jj<NJ; jj+=64) for (k=0; k<NK; k++) for (i=ii; i<min(ii+64,NI); i++) for (j=jj; j<min(jj+64,NJ); j++) C[i][j] += alpha * A[i][k] * B[k][j];. This preserves the original k iteration order and arithmetic, while |
| 2 | rewrite_source | 0.288x  [rewrite: Apply 3D loop tiling: tile i, j, and k with block sizes (BI=64, BJ=64, BK=256). Pre-scale C by beta in a separate clean-up loop. For each (i,j) tile, initialize a scalar accumulator, loop over kk-tiles of k, and inside that iterate over the tile's i and j using the accumulator to sum alpha * A[i][k] * B[k][j] before writing back to C[i][j] once per kk block. This yields cache-fri |
| 3 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): [SMALL_DATASET] Numeric mismatch: max relative error 6.80e-04 at index 2203 (ref=14.7, opt=14.71), epsilon=1.00e-04 |
| 4 | rewrite_source | 1.021x  [rewrite: Apply rectangular i-k tiling with block sizes BI=128, BK=128. Pre-scale C by beta as in the original (each row scaled before its first update). For each i-tile, for each k-tile, compute the inner j loop (entire j dimension) to update the C[i][j] tile. This reuses a 128x128 block of A and a 128xNJ block of B in L2 cache, reduces loads of A and B, while keeping the j-loop contiguou |
| 5 | rewrite_source | 1.133x  [rewrite: Add j-tiling to the existing i-k tiled version. Pre-scale C unchanged. Loop structure: for ii, for kk, for jj, then for i in ii-block, for k in kk-block, for j in jj-block execute C[i][j] += alpha * A[i][k] * B[k][j]. This keeps the i,k,j inner order, preserving FP evaluation order. Use block sizes BI=128, BK=64, BJ=256 to keep A (128x64=8KB), B (64x256=16KB), and C (128x256=32KB |
| 6 | rewrite_source | 1.291x  [rewrite: Start from the current i-k-j tiled version (BI=128, BK=64, BJ=256).  Adjust block sizes to BI=256, BK=64, BJ=128 to shrink the C tile (256×128=256 KB → 128 KB) for better L1 reuse while keeping A and B tiles within L2.  Inside the innermost loop nest (i, k, j), hoist 'alpha * A[i][k]' into a local scalar 'aik' before the j loop, so that the inner statement becomes 'C[i][j] += aik |
| 7 | rewrite_source | 1.198x  [rewrite: Adjust block sizes to BI=64, BK=64, BJ=128 to shrink C tile from 256 KB to 64 KB for better L1d reuse. Preserve the existing hoisting of 'alpha * A[i][k]' into 'aik' and the original loop order (i outside, k middle, j inside). All other structures, including the beta pre-scaling loop and FP evaluation order, remain unchanged.] |
| 8 | rewrite_source | 1.225x  [rewrite: Keep the exact block sizes (BI=256, BK=64, BJ=128), loop order and hoisting as in the current best. Insert #pragma clang loop vectorize(enable) vectorize_width(4) immediately before the innermost j‑loop ('for (j = jj; j < j_end; j++)') to force 256‑bit SIMD vectorisation. No other changes to arithmetic order, tiling or data layout.] |
| 9 | rewrite_source | 0.327x  [rewrite: Keep the loop order, hoisting, and BI=256, BK=64 exactly as in the current best. Change only BJ from 128 to 64 so that the C tile becomes 256×64 = 128 KB (down from 256 KB). This reduces store pressure and may improve L1d reuse without affecting A/B block sizes or floating‑point evaluation order. The inner j‑loop bound becomes min(jj+64, _PB_NJ).] |

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.3482x</b>（基线 18.46 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb004`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：2.0599x　最终确认：**1.3482x**　IQR [1.0053, 1.5884]　base_cv=22.0% best_cv=0.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.192x  [rewrite: Apply cache blocking (loop tiling) to the second loop nest (x[i] = x[i] + beta * A[j][i] * y[j]). Split both i and j dimensions into tiles (e.g., 32x32) so that accesses to A[j][i] are confined to a small column block that fits in L1 cache, reducing capacity misses and improving overall memory bandwidth utilization.] |
| 2 | rewrite_source | 0.616x  [rewrite: Apply loop tiling (cache blocking) to the fourth loop nest (w[i] = w[i] + alpha * A[i][j] * x[j]). Block the outer i dimension into tiles of size B (e.g., 64) so that the x[] vector is loaded once per tile and reused across inner j iterations, reducing repeated loads of x from higher-level caches.] |
| 3 | rewrite_source | 2.060x  [rewrite: Rewrite the fourth loop nest (w[i] += alpha * A[i][j] * x[j]) with i‑dimension tiling of size 256. For each i‑tile, first load w[i] into a local scalar accumulator; then execute the inner j‑loop to accumulate alpha*A[i][j]*x[j] into the accumulator; finally write the accumulator back to w[i]. Use a #pragma clang loop vectorize(enable) on the inner j‑loop to ensure vectorization i |
| 4 | rewrite_source | 1.800x  [rewrite: In the second loop nest (x[i] += beta * A[j][i] * y[j]), inside each i-tile iteration, introduce a local scalar accumulator acc initialized to x[i], accumulate beta * A[j][i] * y[j] into it over the inner j loop, and write acc back to x[i] after the j loop. Keep the existing 32x32 tiling unchanged to maintain A cache locality.] |
| 5 | rewrite_source | 1.581x  [rewrite: In the second loop nest (x[i] = x[i] + beta * A[j][i] * y[j]), introduce a scalar accumulator without adding any tiling or changing the loop structure. For each i, initialise a local scalar acc = x[i], iterate over j accumulating beta * A[j][i] * y[j] into acc, then write acc back to x[i]. Leave the fourth loop’s existing i‑tiling and accumulator unchanged.] |
| 6 | rewrite_source | 1.159x  [rewrite: Apply i‑dimension loop tiling to the first loop nest (A[i][j] += u1[i]*v1[j] + u2[i]*v2[j]) with a tile size of 128. For each i‑tile, load the full v1 and v2 vectors once and reuse them for all i in the tile, reducing repeated vector loads from memory. Keep the existing optimizations on the fourth loop intact.] |
| 7 | rewrite_source | 1.135x  [rewrite: 在第二循环之前动态分配一个 N×N 的临时矩阵 A_trans，通过循环 for i, for j 将 A 转置复制（A_trans[i][j] = A[j][i]）；随后将第二循环改写为使用 A_trans 的连续行访问：for i (外) { scalar acc = x[i]; for j (内) acc += beta * A_trans[i][j] * y[j]; x[i] = acc; }；最后释放临时矩阵。保持第四循环的 i‑分块累加器优化不变。此变换在严格保持原归约顺序的前提下，使内层 j 循环访问连续内存，可有效向量化并消除列访问的缓存缺失。] |
| 8 | rewrite_source | 1.166x  [rewrite: 在第四循环 nest 的内层 j 循环前插入 #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2)，强制编译器生成 512-bit SIMD 指令并交错 2 个向量迭代，以在不破坏累加器语义的情况下提升计算吞吐。保持原有 i-tiling 大小 256 与标量累加器不变。] |
| 9 | rewrite_source | 1.280x  [rewrite: Keep the fourth loop nest’s scalar accumulator and #pragma clang loop vectorize(enable) on the inner j‑loop unchanged. Change the i‑dimension tile size from 256 to 512 (i.e., iterate i in steps of 512, load w[i] into a scalar accumulator, run the full inner j‑loop accumulating alpha*A[i][j]*x[j], then store back). The larger tile reduces outer‑loop branch overhead while still kee |

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>1.3675x</b>（基线 20.97 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb005`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.3966x　最终确认：**1.3675x**　IQR [1.2020, 1.3846]　base_cv=2.4% best_cv=5.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.133x  [rewrite: Split the inner loop over j into two separate loops per i: first compute tmp[i] as the dot product of A[i][*] and x, then compute y[i] as the dot product of B[i][*] and x, then combine with alpha and beta. This isolates the two reductions, allowing each to be auto-vectorized independently without cross-reduction interference, and preserves original accumulation order for each var |
| 2 | rewrite_source | 1.365x  [rewrite: Manually unroll the inner j-loop by a factor of 4 in both separated dot‑product loops (tmp_i and y_i), using scalar accumulators and an explicit remainder loop. The unrolled expression preserves left‑associative addition order, so numerical results are identical. This reduces loop overhead and may improve instruction‑level parallelism.] |
| 3 | rewrite_source | 1.210x  [rewrite: Unroll the outer i-loop by a factor of 2 within each block: process two consecutive rows (i and i+1) in a single iteration, maintaining four independent scalar accumulators (two for tmp, two for y) to preserve original addition order. Keep inner j-loop unrolling as before.] |
| 4 | rewrite_source | 1.397x  [rewrite: Tile the inner j-loop for cache blocking on x: partition the j dimension into tiles of size TILE_J (e.g., 256) and iterate j-tiles in an outer loop; within each tile, iterate over the i-block rows, updating per-row scalar accumulators (tmp_i and y_i) with contributions from A[i][j]*x[j] and B[i][j]*x[j] for j in that tile. After all j-tiles, finalize each row's y[i] = alpha * tmp |
| 5 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpf4a_ln69/gesummv_rewrite.c:105:27: error: duplicate directives 'vectorize(enable)' and 'vectorize(assume_safety)' |
| 6 | rewrite_source | 1.186x  [rewrite: 调整缓存分块参数：将BLOCK_I从64增加到128，将TILE_J从256增加到512。这样每个i-block有更多行，在每一j-tile中x被复用更多次，减少x的加载；同时拉长每个tile的内层循环，分摊循环开销并提高SIMD利用率。保持内层循环unroll_count(4)和融合的累加器不变。] |
| 7 | rewrite_source | 1.317x  [rewrite: In the current best tiled version (BLOCK_I=64, TILE_J=256), add #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2) immediately before the inner j‑loop (the one iterating j from jt to j_max) to force 256‑bit SIMD vectorization with 2‑way interleaving, while keeping the existing unroll_count(4) and scalar accumulators (tmp_i, y_i). This s |
| 8 | rewrite_source | 1.206x  [rewrite: Split the fused inner j-loop inside each tile into two separate loops: first iterate j over the tile to accumulate tmp_i, then iterate again to accumulate y_i, keeping unroll_count(4) and the existing tiling parameters (BLOCK_I=64, TILE_J=256). This isolates the two reductions to improve auto-vectorization while still reusing x[j] from L1 cache within each tile.] |
| 9 | rewrite_source | 1.384x  [rewrite: Based on the current best tiled kernel (BLOCK_I=64, TILE_J=256), insert #pragma clang loop vectorize(enable) interleave(enable) right before the inner j-loop (for j = jt; j < j_max; j++), keeping the existing unroll_count(4) and fused scalar accumulators. Do NOT specify vectorize_width so the compiler can pick the optimal SIMD width. This aims to nudge the backend to vectorize wi |

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>5.4933x</b>（基线 1519.15 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb018`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：6.9520x　最终确认：**5.4933x**　IQR [5.4783, 5.5517]　base_cv=0.5% best_cv=0.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 5.893x  [rewrite: 交换更新 A 的循环中的 i 和 j 顺序：先完成所有 j 的 R[k][j] 计算（保持原样），然后将原两个独立的 i 循环（R 累加和 A 更新）合并重组，外层变 i，内层变 j，使得对 A[i][j] 的访问变为沿行的连续访问。具体地，原代码在 j 循环内先累加 R[k][j] 再更新 A[i][j]，现改为先对 j 循环计算所有 R[k][j]，再对 i 循环、j 内层更新 A[i][j] -= Q[i][k] * R[k][j]。] |
| 2 | rewrite_source | 5.615x  [rewrite: Apply loop tiling (cache blocking) to the A-update loop: split the i-loop (0..M-1) into tiles of size TILE_I (e.g., 64 or 128). For each tile, load the corresponding rows of Q and compute the tile of A updates using the same R[k][j] row, which stays in cache. This improves both temporal reuse of Q[i][k] and R[k][j] and spatial reuse of A[i][j], without changing floating-point ord |
| 3 | rewrite_source | 5.778x  [rewrite: 对A更新循环（外层i，内层j）的内层j进行手动4倍展开：每次迭代计算R[k][j+0..3]与qik的乘积，累加到4个局部标量，然后一次性写回A[i][j..j+3]，末尾处理剩余元素。展开不改变浮点累加顺序（每个A元素独立更新），保持数值一致，同时降低索引开销并提高编译器生成SIMD fused-multiply-add的机会。] |
| 4 | rewrite_source | 6.952x  [rewrite: 对A更新循环（外层i，内层j）进行多维分块（cache blocking）：将i维分成大小为BI的块，将j维（从k+1到N-1）分成大小为BJ的块，引入两层分块循环包围原i和j循环，使得内层计算变为在i块和j块内遍历；BI和BJ建议取128（根据L1大小调整），让R[k][j]的一段和Q[i][k]的一段同时留在L1缓存中被重用，提高访存局部性。不改变浮点计算顺序，保持数值一致。] |
| 5 | rewrite_source | 5.176x  [rewrite: 对 R 累加循环（i=1..M-1 的内层 j 循环，即 R[k][j] += qik * A[i][j]）应用与 A 更新循环相同的多维 cache blocking：将 i 维分成大小为 BI 的块，j 维（k+1 到 N-1）分成大小为 BJ 的块，引入两层分块循环包围原 i 和 j 循环，使得内层计算在 i 块与 j 块内进行，让 A[i][j] 和 Q[i][k] 的子段以及 R[k][j] 留在 L1 缓存中被重用。分块大小与 A 更新循环保持一致（BI=BJ=128），保持浮点累加顺序不变。] |
| 6 | rewrite_source | 5.581x  [rewrite: 在 A 更新循环的现有多维分块结构（外层 i 块循环和 j 块循环）上，将块尺寸 BI 和 BJ 从 128 调整为 64，以更好地匹配每核心约 64KB 的 L1 数据缓存，避免缓存颠簸。同时，保留原有的浮点累加顺序以确保数值一致。可选地，也对 R 累加循环实施相同的小尺寸分块，但优先仅调整 A 更新循环以避免引入额外开销。] |
| 7 | rewrite_source | 5.927x  [rewrite: 对 R 累加循环（i=1..M-1 中的内层 j 循环）进行 j 方向 tiling：引入外层循环 for (int jb = k+1; jb < _PB_N; jb += BJ)，内层保持原始 i 和 j 循环（j 从 jb 到 min(jb+BJ, _PB_N)-1）。BJ 取 128 以与 A 更新分块一致。这一变换使 R[k][j] 的子段（大小为 BJ）在多个 i 行连续更新期间常驻 L1，减少写回和缓存替换，且不改变浮点累加顺序。A 更新循环保持现有的 BI=BJ=128 二维分块不变。] |
| 8 | rewrite_source | 5.693x  [rewrite: 对 R 累加循环（即 i=1..M-1 中的内层 j 循环）执行循环交换并引入标量累加器：将循环重新组织为 outer j (k+1..N-1), inner i (1..M-1)；在 j 循环内声明局部标量 acc，初始化为 Q[0][k]*A[0][j]（替换原来的 if 分支初始化），然后在 inner i 循环内 acc += Q[i][k] * A[i][j]，循环结束后将 acc 写入 R[k][j]。此变换使 R[k][j] 的更新在 j 方向连续化且仅写回一次，同时保持每 j 的 i 累加顺序与原程序一致，数值结果不变。不对 A 更新循环做改动。] |
| 9 | rewrite_source | 6.138x  [rewrite: 对R累加循环（i=1..M-1中的内层j循环）进行j方向分块，并为每个j块分配局部double数组acc[BJ]（初始为0.0）。先计算i=0的贡献：acc[j] = Q[0][k]*A[0][j] 替代原来的if分支。然后对i=1..M-1遍历，内层j在分块内累加acc[j] += Q[i][k]*A[i][j]。最后统一将acc[...]写回R[k][j]对应段。分块大小BJ仍取128，与A更新分块一致，确保局部数组驻留L1。浮点累加顺序与原程序完全一致，数值不变。] |

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.2326x</b>（基线 916.78 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c1_pb027`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.4780x　最终确认：**1.2326x**　IQR [1.2160, 1.4455]　base_cv=0.7% best_cv=9.4%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.866x  [rewrite: Apply loop tiling on the i and j dimensions for both the B-computation and A-computation loops inside each time step. Introduce tile loops (ii, jj) with tile sizes ~32–64, keeping the k-loop intact to preserve unit-stride access. Within a tile, compute B[ii:ii+TI][jj:jj+TJ][:] using A stencil, then later compute A[ii:ii+TI][jj:jj+TJ][:] using B stencil, so that tile data stays in |
| 2 | rewrite_source | 0.901x  [rewrite: Apply 3D loop tiling (blocking) on all three spatial dimensions i, j, k for both the B-computation and A-computation loops inside the time loop. Use tile sizes 32x32x16 (i,j,k) to keep each tile's working set inside L1 cache. The innermost loop stays over k for unit-stride vectorized access. Within each tile, first compute the B block from the corresponding A halo region, then co |
| 3 | rewrite_source | 1.114x  [rewrite: Manually unroll the i loop by a factor of 2 (unroll-and-jam) for both the B-computation and A-computation loops. Inside the unrolled body, compute B[i][j][k] and B[i+1][j][k] while explicitly reusing A neighbor values (e.g., A[i][j][k], A[i+1][j][k]) that appear in both stencils, to reduce total memory loads. Apply the same transformation to the A update loops reusing B. Handle t |
| 4 | rewrite_source | 1.478x  [rewrite: Further unroll-and-jam the j loop by a factor of 2 inside the already i-unrolled loops. Inside the innermost k loop, compute B[i][j][k], B[i][j+1][k], B[i+1][j][k], B[i+1][j+1][k] together to reuse A values loaded for j and j+1 neighbors. Introduce local scalar accumulators for frequently accessed A elements (e.g., a_ij = A[i][j][k], a_i1j = A[i+1][j][k]) to reduce redundant load |
| 5 | rewrite_source | 1.253x  [rewrite: Apply unroll-and-jam on the k loop by a factor of 2 inside the already i,j-unrolled loops. For B computation, compute B[i][j][k], B[i][j+1][k], B[i+1][j][k], B[i+1][j+1][k] and B[i][j][k+1], B[i][j+1][k+1], B[i+1][j][k+1], B[i+1][j+1][k+1] together in the inner loop body, reusing A values that are common across k and k+1 (e.g., A[i][j][k] used by stencils at k and k+1). Introduce |
| 6 | rewrite_source | 1.278x  [rewrite: Increase the i-loop unroll factor from 2 to 4 inside the already j-unrolled loops. Inside the innermost k loop, compute B[i][j][k], B[i][j+1][k], B[i+1][j][k], B[i+1][j+1][k], B[i+2][j][k], B[i+2][j+1][k], B[i+3][j][k], B[i+3][j+1][k] in one go, reusing A loads for i, i+1, i+2, i+3 neighbors. Keep j unroll at 2 to limit register pressure. Handle remainder i interval with a separa |
| 7 | rewrite_source | 0.898x  [rewrite: 对尾循环（i 和 j 的剩余部分）应用与主循环相同的优化：对 i 尾循环（i 从对齐后的值到 N-1）内部，将 j 和 k 的内层循环进行展开和邻居值重用（如 j 展开 2，利用 A 的 j 方向邻居），对 j 尾循环（j 从对齐后的值到 N-1）内部，将 k 内部进行展开重用。同时在这些尾循环中复用标量临时变量来缓存多次出现的 A 和 B 的邻点，以减少尾循环中的重复加载。保留原始串行代码作为残余处理边界情况。] |
| 8 | rewrite_source | 1.222x  [rewrite: Rewrite the time step to apply 2D loop tiling (i, j) and fuse B and A stencil computations within each tile. For each tile (ii,jj) of size 64×64 (adjustable), first compute B[ii:ii+TI][jj:jj+TJ][:] from A halo using the existing i=2,j=2 unrolled stencil with scalar accumulators, then immediately compute A[ii:ii+TI][jj:jj+TJ][:] from the freshly cached B tile using the same unroll |
| 9 | rewrite_source | 0.725x  [rewrite: Insert manual software prefetching hints (__builtin_prefetch) inside the innermost k loop for upcoming A and B elements in the k direction, targeting L1 cache with temporal locality. Also prefetch data for the next i and j iterations when loop bounds permit, to overlap memory access with computation without altering the proven i=2,j=2 unroll-and-jam structure.] |

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>1.0677x</b>（基线 2.30 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb028`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.1950x　最终确认：**1.0677x**　IQR [1.0488, 1.0858]　base_cv=0.7% best_cv=1.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.022x  [rewrite: Manual unroll the two inner loops by factor 4. For the first loop, load A[i-1] through A[i+4] into 6 local scalars, compute B[i] to B[i+3] reusing overlapping inputs, then increment i by 4. For the second loop, analogously read B elements to update A. Hoist the constant 0.33333 into a local volatile variable to avoid repeated FP constant loads.] |
| 2 | rewrite_source | 0.998x  [rewrite: Add __restrict qualifier to A and B pointer parameters to inform the compiler that the arrays do not alias; insert __builtin_prefetch(&A[i+PREFETCH_DIST]) and similarly for B into the manually unrolled loops, with a prefetch distance of 64 double elements (one cache line ahead) to hide memory latency. Also consider splitting the inner loop into blocks of e.g. 256 elements, copyin |
| 3 | rewrite_source | 1.037x  [rewrite: Fuse the two inner loops into a single loop that computes B[i] and then A[i-1] within the same iteration, leveraging the manual unroll-by-4 with scalar reuse. After computing four B values (B[i..i+3]), immediately use them alongside previously computed B[i-2..i-1] to compute A[i-1..i+2]. Preserve original data dependencies and floating-point order. This aims to increase L1 cache  |
| 4 | rewrite_source | 0.507x  [rewrite: Increase the manual unroll factor from 4 to 8 in the fused loop. Load 10 A values (A[i-1] to A[i+8]) into scalars, compute 8 B values (B[i] to B[i+7]), then compute 8 A values (A[i-1] to A[i+6]) reusing the freshly computed B scalars and the pre-loaded A window. Insert __builtin_assume_aligned(A_,64) and __builtin_assume_aligned(B_,64) before the time loop to enable aligned vecto |
| 5 | rewrite_source | 0.550x  [rewrite: Revert to the original separate inner loops from the baseline (no manual unrolling, no fusion). Add #pragma clang loop vectorize_width(8) to both inner loops to force 512-bit SIMD vectorization for double elements. Additionally add #pragma clang loop interleave(enable) interleave_count(4) to hide FP latency, and #pragma clang loop unroll(enable) to let the compiler decide unrolli |
| 6 | rewrite_source | 1.111x  [rewrite: Modify the fused loop body to remove all manual scalar unrolling, keeping only the loop fusion structure (compute B[i] and then A[i-1] within the same i loop). Insert #pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(2) before the fused inner loop. Keep __restrict and volatile constant 'third' unchanged. This lets Clang auto-vectorize with 256-bit vectors ( |
| 7 | rewrite_source | 1.090x  [rewrite: 在当前的融合内层循环（计算B[i]和A[i-1]）外增加一个显式的空间分块循环（blocking）。将i从1到N-1的循环分解为大小为BLOCK（例如128或256，根据L1 cache大小32KB和每个double 8字节计算）的块。块内循环保持不变，块间顺序执行以保证数值一致。保留现有的#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(2)在块内循环上。] |
| 8 | rewrite_source | 1.195x  [rewrite: 将时间步分块大小 TILE 从 4 增加到 8，以减少外层循环迭代次数和分支开销，同时保持融合内层循环（计算 B[i] 和 A[i-1]）和现有的 #pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(2) 不变。确保 tt_end 边界处理正确适应 TILE 不能整除 TSTEPS 的情况。] |
| 9 | rewrite_source | 1.079x  [rewrite: 将时间步分块宏 TILE 从 8 增大到 16，保留融合内层循环和现有的 #pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(2) 不变；保持 tt_end 边界处理适应 TILE 不能整除 TSTEPS 的情形。] |

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>0.8856x</b>（基线 599.09 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c1_pb029`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.2930x　最终确认：**0.8856x**　IQR [0.7371, 1.3387]　base_cv=8.7% best_cv=22.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.293x  [rewrite: 对时间步循环内的两个双层嵌套循环 (i,j) 分别进行 2D 循环分块：引入外层 tile 循环 (i_start, j_start)，块大小设为 TI=64, TJ=64，内层在原 i,j 范围内完成计算。确保边界处理正确（使用 min 限制 tile 结束下标）。] |
| 2 | rewrite_source | 1.057x  [rewrite: Increase tile sizes from TI=64/TJ=64 to TI=128/TJ=128 to better utilize available L2 cache and reduce tile boundary overhead, thereby improving data locality.] |
| 3 | rewrite_source | 0.910x  [rewrite: Adjust tile sizes to TI=96 and TJ=96 to balance cache capacity utilization between 64 and 128. Manually unroll the innermost j loop by a factor of 4 within each tile, computing four consecutive B elements per iteration, to improve register reuse and reduce loop overhead. Ensure correct handling of remainder iterations when tile width is not a multiple of 4.] |
| 4 | rewrite_source | 1.207x  [rewrite: Introduce a local contiguous 2D temporary array `atile[TI+2][TJ+2]` (TI=TJ=64) inside the ii/jj tile loops. Before computing the B tile, copy the corresponding block from A plus a one-element halo into atile, then rewrite the inner stencil to use `atile` instead of the original A. After finishing the B tile, apply the same packing for the B→A update phase. This guarantees all 5-p |
| 5 | rewrite_source | 0.897x  [rewrite: Keep the existing 2D tiling structure but change tile dimensions from TI=64/TJ=64 to TI=128, TJ=32. The inner loops remain unchanged; only the tile stepping constants are modified. Boundary handling via min() is preserved. This shape aims to improve unit‑stride reuse and L1 cache‑line utilization while keeping the per‑tile working set roughly constant.] |
| 6 | rewrite_source | 0.889x  [rewrite: Merge the two separate tiling loops (first computing B from A, then A from B) into a single tiling loop: for each tile (ii, jj), first compute B for all i,j in the tile, then immediately compute A for the same tile using the freshly written B. Keep TI=64, TJ=64. Additionally, add #pragma clang loop vectorize(enable) to the inner j loops to exploit SIMD parallelism.] |
| 7 | rewrite_source | 0.985x  [rewrite: Keep existing 2D tiling (TI=64, TJ=64). Add #pragma clang loop vectorize(enable) vectorize_width(4) and #pragma clang loop unroll_count(2) to both innermost j-loops (the one computing B[i][j] and the one computing A[i][j]) inside the tiled loops. This enforces 256-bit SIMD vectorization and reduces loop overhead, without altering memory access pattern or data dependencies.] |
| 8 | rewrite_source | 0.811x  [rewrite: 在现有2D tiling (TI=64, TJ=64) 内部，将整个tile的A数据packing替换为滑动窗口式行缓存：对于tile内的每个i，将A[i-1]、A[i]、A[i+1]三行在该tile的j范围(含halo)拷贝到连续的局部数组(例如 double local[3][TJ+2] )中，然后内层j循环只使用local数组计算B[i][j]；同样在B→A更新阶段对B数组实施相同滑动窗口缓存。这样可以消除跨行stride访问，同时避免全tile packing的O(TI*TJ)额外内存流量，减轻L1缓存压力，利于SIMD向量化。] |
| 9 | rewrite_source | 0.919x  [rewrite: 在现有 TI=64、TJ=64 空间分块的基础上引入时间分块 (temporal blocking)：将外部时间循环 t 拆分为时间块，每个时间块包含 2 或 4 个原始时间步；对每个空间 tile (ii,jj) 在时间块内连续执行多个更新，即先在本地数组内手动缓存该 tile 的 A 与 B 格点值（含 halo），迭代若干子时间步后再写回全局数组。确保边界 halo 在每次子步后正确更新，保持数值等价。] |

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.1938x</b>（基线 7598.76 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb020`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.3246x　最终确认：**1.1938x**　IQR [1.1927, 1.1944]　base_cv=0.1% best_cv=0.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.086x  [rewrite: Introduce a scalar accumulator 'temp' for A[i][j] in both inner loops: read A[i][j] once into temp, perform all k-loop subtractions on temp, then write back temp (and divide if j<i). This keeps identical FP ordering (same left-to-right subtraction sequence) while reducing alias-exposed loads and stores.] |
| 2 | rewrite_source | 1.146x  [rewrite: Apply cache tiling to the k and j loops inside kernel_lu. Introduce a tile size B (e.g., 64). In the main i-loop, first process the triangular solve (j < i) in tiles of size B: for jt from 0 to i in steps of B. Inside, compute the solve for each j in [jt, min(jt+B, i)) using a k-loop that also advances in tiles of size B (kt from 0 to j in steps of B). For each tile pair, a tempo |
| 3 | rewrite_source | 1.149x  [rewrite: 扩大tile size至128以更好填充L1d缓存。对三角求解部分（j < i）应用双层tiling：对j分块(BJ=128)，对k分块(BK=128)，在内层将A[i][k]提升为局部数组aik[]在j子块内复用，保持每个j的标量累加器temp以保留浮点运算顺序。在rank-k更新的最内层j循环前添加'#pragma clang loop vectorize(enable) vectorize_width(8)'以强制512位向量化并提高ILP。] |
| 4 | rewrite_source | 1.150x  [rewrite: Keep triangular solve as simple scalar accumulator (no blocking) to avoid overhead; in the rank‑k update change tile size to 256 and insert '#pragma clang loop vectorize(enable) vectorize_width(8) interleave_count(2)' before the innermost j‑loop to force wider SIMD and hide FP latency.] |
| 5 | rewrite_source | 1.201x  [rewrite: 对三角求解部分（j < i）应用cache tiling，tile size设为32以减少控制开销；在j的每个tile内保持标量累加器temp，内层k循环也按32分块，并在k块内将A[i][k]提升到寄存器变量以复用；rank-k更新部分保持当前B=256和向量化pragma不变。] |
| 6 | rewrite_source | 1.182x  [rewrite: 将三角求解的分块尺寸从 32 改为 64；将 rank‑k 更新的分块尺寸从 256 改为 128；在 rank‑k 更新的最内层 j 循环前添加 '#pragma clang loop unroll(enable)' 以暴露更多指令级并行。] |
| 7 | rewrite_source | 1.325x  [rewrite: 在 rank-k 更新的每个 jt 块开始处，分配局部数组 double sum[j_end-jt]（栈上VLA），先将 AA[i][jt..j_end-1] 复制到 sum，然后在 k 循环中用 sum 减去 aik*AA[k][j]，最后在 jt 块结束后将 sum 写回 AA[i]。保持当前的 kt/jt 分块和 pragma 不变。] |
| 8 | rewrite_source | 1.271x  [rewrite: 在三角求解的 for (j = k + 1; j < i; j++) 循环前添加 '#pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2)'；在 rank‑k 更新的最内层 for (j = jt; j < j_end; j++) 循环前添加 '#pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2)'，以强制 512 位 SIMD 向量化并应用 interleave 隐藏浮点延迟，提升两个计算密集部分的吞吐。] |
| 9 | rewrite_source | 1.183x  [rewrite: 将 rank‑k 更新的分块尺寸 B 从 64 增大到 256，以提升 A[k][j] 数据的缓存复用并减少循环开销；三角求解部分保持不变。] |

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.2472x</b>（基线 7042.83 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb019`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.2535x　最终确认：**1.2472x**　IQR [1.2262, 1.3086]　base_cv=4.0% best_cv=3.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.000x  [rewrite: Loop tiling on outer i-loop: 将 i 循环 strip-mine 为外层块循环 (tile size 128)，内层 i 循环相同。保持 j 和 k 循环不变。目标：提升 A[k][j] 列访问的缓存局部性。] |
| 2 | rewrite_source | 1.077x  [rewrite: Loop interchange for the upper-triangular j-loop (j = i; j < _PB_N; j++): move the inner k-loop outside so that the loop order becomes k (0..i-1) then j (i..N-1). This makes the innermost loop iterate over j, turning strided column access A[k][j] into contiguous row access, improving cache reuse and enabling potential vectorization.] |
| 3 | rewrite_source | 1.100x  [rewrite: In the current best kernel (which has already interchanged loops so that the innermost loop is the j-loop of the upper-triangular update: for (j = i; j < _PB_N; j++) A[i][j] -= aik * A[k][j];), insert #pragma clang loop vectorize(enable) vectorize_width(8) immediately before that j-loop. Also add #pragma clang loop interleave(enable) interleave_count(2) to hide FP latency. Option |
| 4 | rewrite_source | 1.008x  [rewrite: Implement a right‑looking blocked LU decomposition with tile size 128. In the outer i‑loop (strip‑mined in tiles of 128), factor the current panel rows normally (unchanged). For all rows with index larger than the tile end, move the update operations (A[*][k] /= A[k][k] and the j‑loop multiply‑subtract) into a bulk trailing‑matrix update guarded by the tile boundaries. This clust |
| 5 | rewrite_source | 1.241x  [rewrite: Inside the factorization k-loop, immediately after A[i][k] /= A[k][k], store A[i][k] into a local scalar 'aik' and replace all occurrences of A[i][k] in the following j-loop with 'aik'. Keep the existing #pragma clang loop vectorize(enable) vectorize_width(8) and interleave(enable) interleave_count(2) on the j-loop. This eliminates redundant loads of A[i][k], improves register us |
| 6 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] Optimized output error: NaN in output |
| 7 | rewrite_source | 1.181x  [rewrite: 对因式分解内层的 j 循环施行 cache blocking：将 j 循环 strip-mine 为外层 jj 步长 64 和内层 j 循环，将 jj 循环移到 i 循环外部（即循环顺序变为 for k, for jj, for i in ib-tile, for j in jj-block），使得 A[k][j] 在一个 jj 块内被多个 i 共享，减少内存重载。保留现有标量提升逻辑以及 #pragma omp simd，必要时调整向量化宽度以适应块边界。] |
| 8 | rewrite_source | 1.254x  [rewrite: 在因子分解 k 循环内部，分配一个大小为 N 的临时数组 tmp，将 A[k][j] 从 k+1 到 N-1 的值拷贝到 tmp[j]，然后在所有 i 迭代的 j 循环中使用 tmp[j] 替代 A[k][j] 进行更新运算。保留现有标量提升和 #pragma clang loop vectorize(enable) vectorize_width(8) 等提示。] |
| 9 | rewrite_source | 1.217x  [rewrite: 将临时数组 tmp 的分配移到整个因子分解循环之前（例如在函数入口处使用 alloca 或静态数组一次性分配大小为 _PB_N 的 DATA_TYPE 数组），在每次 k 迭代中只进行从 A[k][j] 到 tmp[j] 的数据拷贝，不再重复分配和释放。同时保留现有的标量提升和向量化 pragma。] |

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>1.1816x</b>（基线 24.73 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb015`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.6088x　最终确认：**1.1816x**　IQR [1.1705, 1.2418]　base_cv=2.9% best_cv=0.8%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.734x  [rewrite: Swap loops in the second kernel (x2 update): move j to the outer loop and i to the inner loop, transforming column-wise traversal of A into row-wise traversal. Also apply register blocking on i (e.g., unroll inner loop by 4) using scalar accumulators to reduce write-back pressure, while preserving the original accumulation order (j across all i sequentially).] |
| 2 | rewrite_source | 1.133x  [rewrite: Apply 2D cache blocking (tiling) to the second kernel (x2 update). Tile the i loop with block size BI (e.g., 32) and the j loop with block size BJ (e.g., 64). The transformed loops iterate over i-tiles and j-tiles, processing a BI×BJ sub-block of A row-wise, accumulating into x2[i] across the tile. This improves spatial locality of A and reuses x2 elements in registers and cache, |
| 3 | rewrite_source | 0.793x  [rewrite: 在对第二个 kernel 已有的 i 维 tiling 基础上增加 j 维 tiling，形成真正的 2D cache blocking：外层 ii 分块 i（BI=64），次外层 jj 分块 j（BJ=64），然后内层遍历 jj 块内的 j 和 ii 块内的 i，即以列优先方式处理 A 的一个 BI×BJ 子块，累加到 x2[i] 上。块大小可通过后续参数调优。] |
| 4 | rewrite_source | 1.609x  [rewrite: 在现有 i 分块（BI=64）基础上，对第二个 kernel 添加 j 维分块（BJ=32）。循环顺序：外层 jj，次外层 ii，内层 j 在 jj 块内，内内层 i 在 ii 块内。关键改进：在 ii 循环前声明标量累加器数组 acc[BI]，将 ii 块内 x2 的更新累积到 acc，ii 块结束后统一写回 x2，以减少存储压力。这样 y_2[j] 和 A 子块在 L1 中被重用，同时避免重复写回。] |
| 5 | rewrite_source | 1.563x  [rewrite: Apply 2D cache tiling to the first kernel (x1 update) analogous to the second kernel’s winning approach: outer tile i by BI (try 64), inner tile j by BJ (try 32), use a scalar accumulator array of size BI for the current ii block to collect partial sums, then write back to x1 after the jj block completes. Loop order: for (ii = 0; ii < _PB_N; ii += BI) { for (jj = 0; jj < _PB_N; j |
| 6 | rewrite_source | 1.504x  [rewrite: 在第一个 kernel（x1 更新）中将 x1[i] 的累加改为显式标量变量 sum：在 i 循环内定义 DATA_TYPE sum = x1[i]，内层 j 循环内 sum += A[i][j] * y_1[j]，循环结束后 x1[i] = sum。第二个 kernel 保持不变。这样消除可能的别名分析失败造成的重复写回，且不改变浮点累加顺序。] |
| 7 | rewrite_source | 1.596x  [rewrite: Apply j‑blocking to the first kernel (x1 update): instead of i outermost, restructure as for (jj=0; jj<N; jj+=BJ) { for (i=0; i<N; i++) { for (j=jj; j<jmax; j++) { x1[i] += A[i][j] * y_1[j]; } } }. This reuses the y_1 segment across all i, keeping x1[i] in a register, without any accumulator array. Keep the second kernel’s 2D tiling+accumulator unchanged.] |
| 8 | rewrite_source | 1.185x  [rewrite: 在第二个kernel（x2更新）的累加器内部循环 `for (i = ii; i < imax; i++) { acc[i-ii] += A[j][i] * y2j; }` 前插入 `#pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)`，强制4倍双精度向量化并2路交错，以掩盖浮点延迟并提升计算吞吐量，同时保持累加器访问的对齐和循环顺序不变。] |
| 9 | rewrite_source | 1.150x  [rewrite: 仅调整第二个kernel的分块参数：将BI从64增大到128，将BJ从32增大到64。外层循环保持不变（jj按BJ分块，ii按BI分块），内层累加器数组大小相应变为128，其他逻辑不变。预期更大的BJ能保留更多y_2元素在L1中，减少y_2的重复读取，更大的BI增加每次分块内的计算量并降低循环开销，从而提升整体性能。] |

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>1.0064x</b>（基线 0.88 ms，hash 校验，29/51 次为正）</summary>

- 任务 `c1_cb011`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0757x　最终确认：**1.0064x**　IQR [0.9790, 1.0189]　base_cv=31.8% best_cv=29.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.578x  [rewrite: Unroll the innermost loop that iterates over neighbors by a factor of 4, and use a scalar accumulator to collect partial updates before writing back to the distance array, reducing store frequency and possibly improving instruction scheduling.] |
| 2 | rewrite_source | 0.448x  [rewrite: Hoist the load of `dist[u]` (the current node distance) outside the inner neighbor loop into a local scalar variable, then use that scalar in all relaxation comparisons and updates instead of repeatedly reading `dist[u]` from memory.] |
| 3 | rewrite_source | 1.076x  [rewrite: Manually unroll the inner neighbor loop by 2 and promote both dist[u] and dist[v] to local scalars within each unrolled iteration, but avoid any cross-iteration accumulator. Insert __builtin_prefetch hints to fetch the next chunk of the neighbor list ahead of use to reduce L1 data cache misses, while keeping the loop structure simple for the compiler's cost model.] |
| 4 | rewrite_source | 1.035x  [rewrite: Hoist the base pointer of the current adjacency row (adj[iNode*NUM_NODES]) into a local scalar before the inner loop, then unroll the inner loop by 4, processing four consecutive neighbors with independent relaxation checks, using the local row pointer to avoid repeated address computation and reduce pressure on the addressing unit, but without any cross-iteration accumulators (w |
| 5 | rewrite_source | 1.000x  [rewrite: Apply cache tiling to the inner relaxation loop (the for‑loop over i that scans all nodes). Split the loop into outer blocks of size BLOCK_SIZE (e.g., 64) so that the nodes array for each block stays in L1 cache across the block’s iterations, while retaining the existing manual unroll‑by‑2 and __builtin_prefetch hints from the current best version. The tiling should not change th |
| 6 | rewrite_source | 0.949x  [rewrite: Within the manually unrolled inner loop (unroll-by-2), load nodes[i].iDist into a local scalar before the conditional comparison and use that scalar for both the NONE-check and the > comparison, then write back the updated value if necessary; this eliminates a second memory load per unrolled iteration while keeping the existing __builtin_prefetch and unroll structure intact.] |
| 7 | rewrite_source | 1.009x  [rewrite: Split the inner relaxation loop over i into two loops: the first loop iterates over all i, performs the distance comparison and updates nodes[i].iDist/iPrev just as before, but instead of calling enqueue immediately, it records the index and new distance of each updated node into local temporary arrays (e.g., int updated_nodes[NUM_NODES] and int new_dists[NUM_NODES], with a count |
| 8 | rewrite_source | 0.944x  [rewrite: Hoist the row pointer of the adjacency matrix for the current node (adj_row = &adj[iNode*NUM_NODES]) to a local variable before the inner loop, replace adj[iNode*NUM_NODES+i] with adj_row[i] inside the manually unrolled loop (keeping unroll-by-2 and __builtin_prefetch hints), to eliminate indexed address arithmetic on each iteration.] |
| 9 | rewrite_source | 1.041x  [rewrite: 在 dijkstra 函数开头添加 static 邻接表（每个节点的邻居索引列表，仅存储 iCost != NONE 的列），使用静态变量和一次性构建。然后内层循环改为遍历当前节点的邻居列表，而不是从 0 到 NUM_NODES。保留现有 unroll-by-2 和 __builtin_prefetch hints，并在遍历邻居列表时使用简单循环（可能不再需要稠密扫描的 ivdep）。该变换可将循环迭代次数从 NUM_NODES 降低到平均出度，极大减少分支和地址计算，同时保持数值正确性。] |

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>0.9938x</b>（基线 2.00 ms，hash 校验，24/51 次为正）</summary>

- 任务 `c1_cb012`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2724x　最终确认：**0.9938x**　IQR [0.9733, 1.0238]　base_cv=12.8% best_cv=13.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpg4ld5nlw/network_patricia_rewrite.c:146:10: error: use of undeclared identifier 'max_align_t' |
| 2 | rewrite_source | 1.138x  [rewrite: Replace the per-iteration malloc calls for ptree, ptree_mask, and MyNode with a custom slab allocator. Outside the main loop, allocate a large buffer (e.g., 64KB). Inside the loop, bump-allocate aligned blocks (align to 16 bytes using simple pointer arithmetic) for the three structures. When the buffer is exhausted, allocate another slab of the same size. Avoid max_align_t; use a |
| 3 | rewrite_source | 1.091x  [rewrite: 将 ptree 节点、ptree_mask 和 MyNode 的 slab 分配及初始化从主循环开头延迟到 else 分支（插入路径）内部，当 pat_search 命中时完全不执行任何分配或初始化，同时将 if (!p) 的错误检查移至插入分支内，保留原语义。] |
| 4 | rewrite_source | 1.095x  [rewrite: 将主循环内三次独立的 slab 分配和清零合并为一次连续分配：计算 struct ptree、struct ptree_mask 和 struct MyNode 的总大小，在 slab 中一次性地 bump 分配该总空间，然后调整 p、p->p_m 和 pm->pm_data 指针指向内部偏移，使用单次 memset 清零整块区域，从而减少 bump 操作次数和 memset 调用次数，提升缓存局部性。] |
| 5 | rewrite_source | 0.995x  [rewrite: 将 pat_search 和 pat_insert 函数定义移动到 kernel_network_patricia 函数之前，并添加 static inline 关键字，强制编译器将它们内联到主循环中，消除函数调用开销，并为编译器提供更完整的优化上下文。] |
| 6 | rewrite_source | 1.272x  [rewrite: Convert the Patricia trie implementation (pat_search and pat_insert) from pointer‑based linking to array‑based linking: define a global dynamic array of ptree nodes (or reuse the slab allocator to supply contiguous blocks) and replace child pointers p_left/p_right with integer indices. Adjust the recursive/iterative traversal in both functions to use array indices instead of poin |
| 7 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpygd885qd/network_patricia_rewrite.c:142:14: error: member reference base type 'char' is not a structure or union |
| 8 | rewrite_source | 失败 [rewrite_source] optimized run timed out |
| 9 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpvki932ku/network_patricia_rewrite.c:135:31: error: call to undeclared function 'ntohl'; ISO C99 and later do not support implicit function declarations [-Wimplicit-function-declaration] |

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>1.0665x</b>（基线 4077.00 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c1_pb024`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0802x　最终确认：**1.0665x**　IQR [1.0652, 1.0727]　base_cv=0.2% best_cv=0.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.951x  [rewrite: Apply loop tiling for the i and j loops. Divide the N×N table into B×B blocks (B=64 to fit in L1 cache). Process blocks diagonally from bottom-right to top-left to honor dependencies. Within each block, perform the original DP computation on local indices. Handle cross-block dependencies (table[i+1][j], table[i][j-1], table[i+1][j-1]) by reading from the already-computed neighbor |
| 2 | rewrite_source | 1.033x  [rewrite: Introduce a scalar accumulator 'best' to hold the current best score for table[i][j]. Perform all max_score comparisons on 'best' and write back only once after the k‑loop. Then manually unroll the innermost k‑loop by a factor of 4 (with a scalar remainder loop) to reduce loop branching overhead and improve instruction scheduling.] |
| 3 | rewrite_source | 1.025x  [rewrite: Remove the redundant condition j-1>=0 (always true for j>=i+1>=1). Hoist the i+1<_PB_N check: split the i-loop into a main loop for i < _PB_N-1 where the table[i+1][j] references are safe, and a tail iteration for i == _PB_N-1 where those accesses are skipped. In the main loop, eliminate the corresponding if guards, keeping only the essential i<j-1 condition. This reduces per-ite |
| 4 | rewrite_source | 0.954x  [rewrite: Swap the i and j loops: make j the outer loop from 1 to _PB_N-1 (incrementing), and i the inner loop from j-1 down to 0 (decrementing). This preserves the DP dependency order (i+1 and j-1 are already computed) while improving temporal locality of the column access pattern in the k-loop (table[k+1][j]) since j is now fixed for all inner i iterations. Retain the existing scalar acc |
| 5 | rewrite_source | 1.032x  [rewrite: Insert '#pragma clang loop vectorize(enable) vectorize_width(4)' and '#pragma clang loop interleave(enable) interleave_count(2)' immediately before the k‑loop. Also hoist the call to match(seq[i], seq[j]) into a local scalar variable before the inner if‑block, so the function is invoked only once per (i,j) pair. Keep the existing scalar accumulator and manual k‑loop unroll.] |
| 6 | rewrite_source | 1.045x  [rewrite: Expand scalar accumulation to all updates of table[i][j]: initialize best = table[i][j], then accumulate the three candidate max scores (from j-1, i+1, and i+1,j-1 with match) into best using max_score, before running the k‑loop. Finally write best back to table[i][j] once. Eliminates redundant writes before the k‑loop while preserving existing unroll and accumulator inside the k |
| 7 | rewrite_source | 0.937x  [rewrite: Based on the current best kernel, manually unroll the innermost k‑loop by a factor of 8 instead of 4. The unrolled body computes the maximum of the current best and max_score(best, table[i][k] + table[k+1][j]) for each of the 8 (or fewer) consecutive k‑values, then updates ‘best’. A scalar remainder loop handles any remaining iterations. Retain all existing optimizations: scalar  |
| 8 | rewrite_source | 1.014x  [rewrite: Based on the current best kernel (full scalar accumulation on all updates + k-loop unroll by 4), insert '#pragma clang loop vectorize(enable) vectorize_width(4)' and '#pragma clang loop interleave(enable) interleave_count(2)' immediately before the innermost k-loop (for (k=i+1; k<j; k++)). Keep the existing manual unroll and scalar accumulator unchanged. This forces SIMD vectoriz |
| 9 | rewrite_source | 1.080x  [rewrite: 在 kernel 函数开头声明 DATA_TYPE (* restrict tbl)[N] = table; 并用 tbl 替换函数体内所有对 table 的引用，告知编译器 tbl 是唯一访问表内存的指针，使内部 k‑loop 归约可被向量化。保留现有的全量 scalar accumulator 和 k‑loop unroll‑4。] |

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>0.9684x</b>（基线 1.17 ms，hash 校验，3/51 次为正）</summary>

- 任务 `c1_cb013`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.2112x　最终确认：**0.9684x**　IQR [0.9544, 0.9789]　base_cv=6.1% best_cv=5.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.089x  [rewrite: 交换双重循环次序：将外层 i（search_strings）与内层 j（find_strings）互换，使得外层循环遍历 find_strings，内层循环遍历 search_strings，从而内层连续访问 search_strings 数组，提升缓存局部性，并可能复用模式预处理。] |
| 2 | rewrite_source | 1.097x  [rewrite: 将Aho-Corasick自动机的goto函数从trie的孩子链表查找改为预计算的二维整型转移表goto_table[max_nodes][256]。在构建完trie和失败链接后，通过BFS填充完整的转移表，使得对于任意节点state和字符c，下一个状态直接由goto_table[state][c]给出。同时为每个节点构建一个直接输出链表或输出元组，避免扫描时通过fail链递归收集匹配模式。搜索主循环改为：for each character c in line: state = goto_table[state][c]; if (output_idx[state] != -1) matched[output_idx[state]] = 1; 等，并遍历输出链标记所有匹配。这将消除内层while搜索子节点和fail回溯链，显著精简热循环并提升缓存友好性。 |
| 3 | rewrite_source | 0.963x  [rewrite: 在构建自动机阶段，为每个节点构建一个整数链表（out_head[node] 和 out_next[...]）来预收集该节点及其 fail 链上所有模式的输出索引。具体：初始化 out_head 全为 -1；遍历所有节点，若 out[node]!=-1，创建一个链表节点记录该匹配索引，并将其插入该节点的链表尾部；同时，对于节点 node，在计算 fail[node] 后，若 fail[node] 的输出链表非空，则将其链接到 node 的输出链表末尾（或以预计算形式：遍历 fail 链收集所有输出后统一构造）。扫描热循环改为：state = goto_table[state * 256 + c]; for (int idx = out_head[state]; idx != -1; idx = out_next[idx]) matched[idx] = 1 |
| 4 | rewrite_source | 1.206x  [rewrite: 在构建完goto_table后，为每个AC节点预计算其所有匹配模式索引的连续列表。使用两个数组out_start[node]和out_idx[]：遍历所有节点，将每个自身out[node]以及沿fail链继承的匹配索引依次追加到out_idx中，并记录每个节点的起始索引和长度（或结束索引）。在扫描热循环中，将原有while(tmp!=root) fail链追逐替换为for(k=out_start[state]; k<out_start[state]+out_count[state]; k++) matched[out_idx[k]]=1;，从而消除fail链指针跳转和间接分支，改善数据局部性并缩短关键路径。] |
| 5 | rewrite_source | 1.052x  [rewrite: 将 matched 数组从 char* 改为 int* matched_epoch，并维护一个 int epoch 计数器。每行开始时 epoch++，扫描状态下遇到匹配模式 idx 时，设置 matched_epoch[idx] = epoch 替代 matched[idx] = 1。在输出循环中，条件由 if (matched[j]) 改为 if (matched_epoch[j] == epoch)。移除原来的 memset(matched, 0, i2) 调用，彻底消除每行 O(i2) 的清零写入。] |
| 6 | rewrite_source | 0.973x  [rewrite: 对主扫描循环进行双字符展开：每次迭代读取两个字符c1和c2，分别执行状态转移和匹配标记（先c1后c2），以隐藏转移延迟并减少循环开销。处理尾部剩余单个字符。不改变任何数据结构或输出语义。] |
| 7 | rewrite_source | 1.039x  [rewrite: 将 goto_table 的类型从 int* 改为 short* (int16_t)，并将所有用于状态索引的变量（state, child, next_sibling 中的子节点索引等）从 int 改为 short。同时将 out_start、out_count、out_idx 中的状态索引等调整为 short（模式索引 j 可保持 int 以避免溢出）。此变换使 goto_table 内存占用减半，提高 L1/L2 缓存命中率，从而加速主扫描循环。需在构建和访问处统一类型，确保无符号/范围截断。] |
| 8 | rewrite_source | 1.047x  [rewrite: 在扫描循环之前，将 goto_table、out_idx、out_start、out_count、matched 等核心指针分别赋给带有 __restrict 属性的局部指针变量（例如 int *restrict goto_table_local = goto_table;），然后在整个扫描和输出循环中只使用这些局部变量。该变换不改变任何数据结构或算法，仅提示编译器这些指针不与其他内存别名，从而消除冲突假设，允许更高效的指令调度和向量化生成。] |
| 9 | rewrite_source | 1.211x  [rewrite: 在 current_best 基础上，将 matched 数组类型从 char* 改为 uint16_t* matched_epoch（并 calloc 分配），引入 uint16_t epoch 计数器。每行开始时 epoch++，扫描内匹配时将 matched_epoch[idx] = epoch 替代 matched[idx]=1；输出循环将 if(matched[j]) 改为 if(matched_epoch[j]==epoch)，移除 memset 调用。同时，为 goto_table、out_idx、out_start、out_count 等核心指针创建 __restrict 局部变量并在热循环中使用。] |

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>1.0000x</b>（基线 1.25 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb021`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.2304x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.092x  [rewrite(utils/BF_encrypt): Add 'const' qualifier to the 'key' parameter and declare local pointers 'p' and 's' as 'const restrict' to indicate that the key arrays are never modified and do not alias data. This allows the compiler to hoist S-box reads and keep round keys in registers, potentially reducing memory traffic and latency within the round function chain.] |
| 2 | rewrite_source | 0.974x  [rewrite(utils/BF_encrypt): In BF_encrypt, copy the contents of key->P and key->S into local stack arrays p_local[18] and s_local[1024] at the beginning of the function, then replace all references to p[i] and s[i] with p_local[i] and s_local[i] throughout the encryption/decryption chains. This transforms indirect pointer accesses into direct local array accesses, allowing the compiler to  |
| 3 | rewrite_source | 0.990x  [rewrite(utils/BF_encrypt): In BF_encrypt, copy all 18 elements of key->P into a local array p_local[18] at function entry, then replace every occurrence of p[i] with p_local[i] throughout the encrypt and decrypt chains. Keep the const‑restrict pointer s for key->S untouched (no copy) and leave all other code identical. This eliminates any residual compiler reload of the p base pointer whi |
| 4 | rewrite_source | 1.230x  [rewrite(utils/BF_encrypt): Add 'static inline' qualifier to the BF_encrypt function definition so that every call site in the same translation unit becomes inlined, eliminating call overhead and enabling S‑box base address reuse across consecutive encryption/decryption calls within loops.] |
| 5 | rewrite_source | 1.002x  [rewrite(utils/BF_encrypt): At the start of BF_encrypt, after setting s = &(key->S[0]), create four const‑qualified local pointers: s0 = s, s1 = s + 0x100, s2 = s + 0x200, s3 = s + 0x300. Replace every S‑box access inside BF_ENC (and all macro expansions in both encrypt/decrypt branches) from S[0x100+idx] to s1[idx], S[0x200+idx] to s2[idx], etc., so that the compiler sees constant base ad |
| 6 | rewrite_source | 0.526x  [rewrite(utils/BF_encrypt): Add 'restrict' qualifier to the 'data' parameter of BF_encrypt so that the compiler knows the output buffer does not overlap with key->P or key->S. This may enable better instruction scheduling between S‑box loads and result stores.] |
| 7 | rewrite_source | 0.914x  [rewrite(utils/BF_encrypt): Manually inline the BF_ENC macro inside both encrypt/decrypt branches, breaking each round into stages: pre‑compute all four S‑box indices from l (which are available early), then perform the four S‑box loads (which are independent of each other and of the XOR), and finally combine them with p[i] and r. Spread the loads and arithmetic across multiple statements  |
| 8 | rewrite_source | 1.083x  [rewrite(utils/BF_encrypt): Add __attribute__((always_inline)) to the definition of BF_encrypt (which is already 'static inline') to guarantee full inlining at every call site, eliminating any remaining call overhead under inlining budget pressure and expanding the instruction scheduling window across the caller's loop.]content empty, falling back to reasoning_content on attempt 1 |
| 9 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp5xm63czl/rw_utils_utils_shadow/polybench.c:652:2: error: expected identifier or '(' |

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>1.0000x</b>（基线 1.19 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb020`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0963x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.096x  [rewrite(utils/BF_encrypt): 将 encrypt 和非 encrypt 两条路径各自提取为 static 内联函数，BF_encrypt 根据 encrypt 参数调用对应函数，以消除函数体内的 if-else 分支，允许编译器为每条路径独立生成更优的指令调度和寄存器分配。] |
| 2 | rewrite_source | 1.020x  [rewrite(utils/BF_encrypt): 为 BF_encrypt 的函数参数添加 restrict 和 const 限定（data 和 key 加上 restrict，key 加上 const），并将 BF_encrypt 声明为 __attribute__((always_inline, hot))，同时确保其调用的加解密函数也有 always_inline 和 hot 属性，以消除别名分析障碍、强制内联并优化指令缓存布局。] |
| 3 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpvn2jgjeo/rw_utils_utils_shadow/polybench.c:720:1: error: expected identifier or '(' |
| 4 | rewrite_source | 1.096x  [rewrite(utils/BF_encrypt): 基于当前最优版本（已将 encrypt 与非 encrypt 路径分为 static inline 函数），为 BF_encrypt 的 data 参数添加 restrict 限定符、为 key 参数添加 const 限定符，但不添加 always_inline 属性（修复上次因语法错误导致编译失败的问题，确保限定符放置在参数列表的正确位置）。期望通过消除指针别名假设，允许编译器对 data 和 key 的访问进行更积极的指令重排和寄存器分配。] |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp3nfo82cv/rw_utils_utils_shadow/blowfish.h:89:26: note: previous declaration is here |
| 6 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmps3rsclks/rw_utils_utils_shadow/blowfish.h:89:26: note: previous declaration is here |
| 7 | rewrite_source | 1.057x  [rewrite(utils/BF_encrypt): Make BF_encrypt_enc and BF_encrypt_dec const-correct: change their key parameter type from (BF_KEY*) to (const BF_KEY*), use const-qualified pointers when accessing key->P and key->S inside these functions, and remove the unnecessary (BF_KEY*) cast in BF_encrypt call. Expect improved alias analysis enabling more aggressive register allocation and load eliminatio |
| 8 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpwz8nhso1/rw_utils_utils_shadow/blowfish.h:89:26: note: previous declaration is here |
| 9 | rewrite_source | 0.989x  [rewrite(utils/BF_encrypt): 在 BF_encrypt 的 key 参数上添加 restrict 限定符：将参数类型从 'const BF_KEY *key' 改为 'const BF_KEY *restrict key'，告知编译器 key 指针是唯一指向该 BF_KEY 的入口，消除 key->P 和 key->S 数组访问的别名顾虑，使编译器能更早地将这些指针提升到寄存器中，减少重复加载。不修改其他函数或局部变量，避免编译错误。] |

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>0.9974x</b>（基线 2.11 ms，hash 校验，22/51 次为正）</summary>

- 任务 `c1_cb014`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.6829x　最终确认：**0.9974x**　IQR [0.9783, 1.0275]　base_cv=8.8% best_cv=8.9%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.924x  [rewrite: 对 key 解析 while 循环应用查表法加速十六进制字符转换，消除 if-else 分支；使用局部标量变量累积 key 字节值，仅在每两个字符后写入 key 数组，减少内存存储操作。] |
| 2 | rewrite_source | 1.006x  [rewrite: 重写 key 解析 while 循环：使用位运算技巧 (c & 0xF) + (c >> 6) * 9 直接转换 ASCII 十六进制字符为数值，消除原 if-else 分支和 toupper 函数调用，减少分支预测失败开销和函数调用开销，并使用标量局部变量累积 key 字节，每两个字符写入 key 数组。] |
| 3 | rewrite_source | 失败 [rewrite_source] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpm3wkk_t0/security_rijndael_decode_rewrite.c:214:11: error: use of undeclared identifier 'uint64_t' |
| 4 | rewrite_source | 1.071x  [rewrite: 将解密while(1)循环中的逐字节XOR循环 `for(i=0;i<16;++i) outbuf[i]^=bp2[i];` 手动完全展开为16条独立的异或赋值语句（outbuf[0]^=bp2[0]; outbuf[1]^=bp2[1]; ...），消除循环归纳变量与条件分支，同时为编译器提供更清晰的连续操作模式，以便其自动生成更宽的数据通路指令。] |
| 5 | rewrite_source | 1.683x  [rewrite: 将局部数组 inbuf1, inbuf2, outbuf 声明为 __attribute__((aligned(16))) char inbuf1[16];  （同样对齐 inbuf2 和 outbuf），向编译器承诺这些缓冲区 16 字节对齐，使其能够在对已展开的 XOR 操作和 decrypt 调用中使用对齐的 128 位 load/store 指令，避免保守的未对齐访存惩罚。] |
| 6 | rewrite_source | 失败 [rewrite_source] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpoketsyd2/security_rijndael_decode_rewrite.c:190:9: error: use of undeclared identifier '__m128i' |
| 7 | rewrite_source | 失败 [rewrite_source] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp29097j8y/security_rijndael_decode_rewrite.c:190:9: error: use of undeclared identifier '__m128i' |
| 8 | rewrite_source | 失败 [rewrite_source] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpqamx6s5q/security_rijndael_decode_rewrite.c:217:13: error: use of undeclared identifier 'uint64_t' |
| 9 | rewrite_source | 0.867x  [rewrite: 将解密 while(1) 循环内手动展开的 16 条逐字节异或赋值（outbuf[0]^=bp2[0]; ... outbuf[15]^=bp2[15];）替换为两次 8 字节异或：使用 unsigned long long* 指针（无需额外头文件）将 outbuf 和 bp2 转换为 64 位整型指针，执行 out_q[0] ^= bp2_q[0]; out_q[1] ^= bp2_q[1]; 利用已有的 __attribute__((aligned(16))) 确保对齐，减少指令数并生成更宽的 SIMD 操作。] |

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>1.0052x</b>（基线 1.61 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c1_cb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3522x　最终确认：**1.0052x**　IQR [0.9874, 1.0271]　base_cv=8.7% best_cv=7.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.352x  [rewrite: 使用静态256元素查找表（有效十六进制字符映射到4位值，无效字符标记为-1）替代 while 循环内的条件分支；同时将奇偶存储逻辑与查表合并，减少分支和位操作开销，保持输出语义不变。] |
| 2 | rewrite_source | 0.921x  [rewrite: 对 while 循环进行展开：每次迭代处理两个十六进制字符（即四个字节的解析），通过手动 2x 展开减少循环控制开销和索引更新。使用静态查找表同时查两个字符，累加为字节并存储，保持奇偶存储逻辑的等价性。需处理奇数字节的情况。] |
| 3 | rewrite_source | 1.014x  [rewrite: 将 while 循环内的奇偶存储逻辑（if(i++ & 1) key[i/2-1] = ...）替换为基于标志位 n 的高低半字节状态机：n=0 时暂存高四位，n=1 时合并存入 key 并重置。用独立计数器 k 记录字节索引，保留 i 作为总字符计数以满足后续长度校验。] |
| 4 | rewrite_source | 0.648x  [rewrite: 将十六进制解析的 while 循环拆分为两个顺序循环：第一个循环遍历输入字符串，使用静态查找表将每个有效十六进制字符转换为 4-bit nibble，存入临时 nibble 数组（长度 64），遇到无效字符则跳转错误处理；第二个循环以步长 2 遍历 nibble 数组，将每对 nibble 组合为一个字节存入 key，并使用独立索引 j 避免除法。这样消除了单个循环内的状态依赖，使转换部分成为无分支的数组映射，更容易被编译器自动向量化（例如 128/256 位 SIMD 一次处理多个字符），同时保持语义等价。] |
| 5 | rewrite_source | 1.049x  [rewrite: Use uint64_t bitwise operations to convert 8 hex characters per iteration via static lookup table, accumulate nibbles in a 64-bit word, then split into 4 bytes and store them to key array using a stride. Replace while loop with a for loop over blocks of 8, handling remainder separately, to reduce loop overhead and expose more instruction-level parallelism. Ensure error checking i |
| 6 | rewrite_source | 1.058x  [rewrite: 在函数开头将 cp 声明为 const char * __restrict cp = argv[4];，并将 key 写入改用局部指针 char * __restrict kp = key; 递增，避免 i/2-1 的除法索引运算。保留 i 的奇偶控制但完全以指针操作存储字节，降低地址计算开销并帮助编译器推断无别名，进而可能对查表循环实施向量化。] |
| 7 | rewrite_source | 1.010x  [rewrite: 使用SSE2内建函数（_mm_loadu_si128加载16个字符，_mm_shuffle_epi8并行查表获得nibble，再通过移位和位运算组合成字节存储到key）对hex解析循环进行向量化改写，保留错误检测逻辑（若向量中发现无效字符则退回标量处理并报告错误），同时用restrict限定输入指针帮助编译器优化。] |
| 8 | rewrite_source | 0.678x  [rewrite: 在当前最优查表版本基础上，将输入指针 cp 声明为 const char *__restrict cp = argv[4];，输出字节存储改用局部指针 char *__restrict kp = key; 递增写入，消除 i/2-1 的除法索引，同时保留原有的查表分支消除和奇偶存储逻辑，以允许编译器更激进地向量化或指令调度。] |
| 9 | rewrite_source | 1.177x  [rewrite: 在当前最优查表版本基础上，仅将静态查找表的元素类型从 int 改为 int8_t（signed char），以减少单次查表的内存加载宽度，降低指令 cache 占用，同时保持所有功能语义和错误检测不变。不引入任何新的控制流、临时数组或指针限定词，最大限度保证不破坏现有优化。] |

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>1.0000x</b>（基线 1.89 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb016`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0206x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp8w4c6oph/rw_utils_utils_shadow/polybench.c:763:1: error: unknown type name 'We' |
| 2 | rewrite_source | 0.480x  [rewrite(utils/sha_stream): 从 polybench.c 中提取 sha_update 的完整实现，手动内联到 sha_stream 的 while 循环中，保留对 fread 返回长度不为 BLOCK_SIZE 的处理逻辑。内联后，对内部分块处理循环（通常为 for 循环，每次处理 64 字节）使用 #pragma clang loop unroll(4) 或 unroll(8)，并声明局部 register 变量暂存 sha_info 的状态数组，减少冗余访存。] |
| 3 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpypat8i_a/rw_utils_utils_shadow/polybench.c:769:5: error: use of undeclared identifier 'uint32_t' |
| 4 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpbt27ubch/rw_utils_utils_shadow/polybench.c:770:48: error: no member named 'count' in 'SHA_INFO' |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpt8tcbetu/rw_utils_utils_shadow/polybench.c:768:5: error: use of undeclared identifier 'uint32_t' |
| 6 | rewrite_source | 1.001x  [rewrite(utils/sha_stream): 将 sha_stream 中 fread 的读入块大小从 BLOCK_SIZE 改为一个更大的值（例如 4096 或 8192），同时在循环内新增一个内层 while 循环，将每次读入的数据按原 BLOCK_SIZE 分块逐一传递给 sha_update，直到耗尽本次读取的字节数。这保持了原始流式语义，但减少了 fread 调用的次数和主循环的迭代次数，有望降低调用开销和分支开销。] |
| 7 | rewrite_source | 0.996x  [rewrite(utils/sha_stream): 将 sha_update 函数的定义改为 static inline __attribute__((always_inline))，前置到 sha_stream 之前，并将输入参数 data 声明为 const BYTE *restrict；同时将 sha_init 和 sha_final 也用 same 属性内联，以消除调用开销。不对任何循环添加手工 pragma，保留编译器自动优化能力。] |
| 8 | rewrite_source | 1.021x  [rewrite(utils/sha_stream): 在 sha_stream 快速路径中，当整个文件被一次性读入到动态分配的缓冲区后，不再按 BLOCK_SIZE 分片循环调用 sha_update，而是直接将整个缓冲区指针和长度 n 作为一次 sha_update 的输入，消除最后的函数调用循环开销。] |
| 9 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpoowv4w06/rw_utils_utils_shadow/polybench.c:778:17: error: use of undeclared identifier 'uint32_t' |

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>0.9943x</b>（基线 13404.26 ms，numeric 校验，1/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c1_pb030`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0120x　最终确认：**0.9943x**　IQR [0.9938, 1.0029]　base_cv=0.0% best_cv=0.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.012x  [rewrite: 对 i 循环进行 cache blocking (tiling)：将 i 循环从 1 到 N-2 分割为大小为 TILE_I 的多个块 (for(int ii=1; ii<N-1; ii+=TILE_I))，内层 i 循环遍历当前块，最内层仍是完整的 j 循环。选择合适的 TILE_I (如 64 或 128) 使一个 tile 内多行数据留在 L1 cache 中，以提升时间步内和跨时间步的访存局部性。] |
| 2 | rewrite_source | 0.999x  [rewrite: 对 i 和 j 循环同时进行 2D cache tiling：将 (i,j) 空间划分成 TILE_I x TILE_J 的矩形块（如 32x32），外层按行列顺序遍历块，内层遍历块内 (i,j) 元素。由于 Gauss-Seidel 依赖顺序需要逐行更新，块的处理必须保持从左上到右下的依赖方向，即外层 ii 和 jj 顺序遍历，内层先 i 后 j。这样可以确保每块的数据（当前行、上行、下行及其邻居）整个被保留在 L1 缓存中，最大化空间上的数据复用。] |
| 3 | rewrite_source | 0.887x  [rewrite: 在现有 i 循环 cache blocking 的基础上，对最内层 j 循环进行手动展开（unroll 2），将原本迭代 j 的循环体拆分为每两次迭代一组，显式写出计算 A[i][j] 和 A[i][j+1] 的代码，并调整 __builtin_prefetch 的偏移量以匹配展开后的步长，从而减少循环分支预测开销并可利用标量寄存器缩短部分依赖链。] |
| 4 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): [SMALL_DATASET] Numeric mismatch: max relative error 1.00e-02 at index 167 (ref=0.43, opt=0.42), epsilon=1.00e-04 |
| 5 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpyijs2u_t/seidel-2d_rewrite.c:68:45: error: use of undeclared identifier 'TILE_I' |
| 6 | rewrite_source | 1.004x  [rewrite: 调整 i 方向 cache blocking 的 tile 大小 TILE_I：在 current_best 源码基础上，将 TILE_I 从现有值（推测为 64）改为 128，使每个 tile 处理更多连续行，充分利用 1.3 MiB L1 cache 容纳更大的工作集，提升 i 方向的数据复用并减少 tile 控制开销。j 循环及其余结构完全保持不变。] |
| 7 | rewrite_source | 0.999x  [rewrite: 在函数参数声明中为二维数组 A 添加 restrict 限定符（例如将 DATA_TYPE POLYBENCH_2D(A,N,N,n,n) 转换为 DATA_TYPE (* restrict A)[N] 的形式），以明确行指针之间不会别名。同时保持现有 i 循环 cache blocking 结构不变，去除或调整 __builtin_prefetch 以避免与编译器自动预取冲突。预期通过改善指令调度和寄存器分配获得额外加速。] |
| 8 | rewrite_source | 1.000x  [rewrite: 在现有i循环cache blocking基础上，将TILE_I大小从当前值（推测为64）改为32。代码中定义const int TILE_I = 32;，外层循环for(int ii=1; ii<_PB_N-2; ii+=TILE_I)保持原样，内层i循环上限调整为min(ii+TILE_I, _PB_N-2)。其余部分（j循环、prefetch、算术）完全不变。期望通过更精确匹配L1 cache容量，减少行间数据替换，提升访存局部性。] |
| 9 | rewrite_source | 1.006x  [rewrite: 在现有 i 循环 cache blocking (TILE_I=64) 的基础上，将每个 tile 内的 i 循环拆分为两个顺序区间：前半部分在 i 循环中无条件执行 __builtin_prefetch(&A[i+2][0],0,3)（循环范围 ii 到 min(ii+TILE_I, _PB_N-3)），后半部分无 prefetch（范围 min(ii+TILE_I, _PB_N-2) 到 min(ii+TILE_I, _PB_N-2) 实际上就是剩余行），从而完全移除原始代码中每行迭代的 if (i+2 < _PB_N) 条件检查。其余结构（j 循环、算术表达式、TILE_I 定义）保持不变，确保数值完全相同。] |

</details>

<details><summary><b>symm</b> — 中位加速比 <b>4.6123x</b>（基线 951.00 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb006`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：5.5009x　最终确认：**4.6123x**　IQR [4.6042, 4.8076]　base_cv=2.2% best_cv=0.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.979x  [rewrite: Loop interchange: for each i, swap the j and k loops so that j becomes the innermost loop. Introduce a scalar temporary array temp2[N] (or use a stack buffer) to accumulate the dot-product per j. In the new inner j-loop, iterate over j to update C[k][j] += alpha*B[i][j]*A[i][k] and temp2[j] += B[k][j]*A[i][k] using contiguous row-wise accesses to B[i], B[k], and C[k]. Then finali |
| 2 | rewrite_source | 4.579x  [rewrite: Apply loop tiling (cache blocking) to the outer i and j loops. Choose block sizes BI and BJ (e.g., 32 or 64) such that a BI×BJ block of B and a BI×BJ block of C fit in L1 cache. Then compute the tile: for i=0 to M-1 by BI, for j=0 to N-1 by BJ, execute the original inner three loops but only for the tile index ranges, using scalar temporaries as needed. This reuses the B and C su |
| 3 | rewrite_source | 5.410x  [rewrite: Apply two-level loop tiling to enhance cache reuse. Block the i-loop with tile size BI (e.g., 64) and the j-loop with tile size BJ (e.g., 256). For each tile (ii, jj), allocate local accumulation arrays for the C tile and perform the original triangular update (k-loop over 0..i-1) only for indices within the current tile. After finishing the k-loop, finalize the C[i][j] values fo |
| 4 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): [SMALL_DATASET] Numeric mismatch: max relative error 1.67e-01 at index 0 (ref=22.42, opt=26.91), epsilon=1.00e-04 |
| 5 | rewrite_source | 5.309x  [rewrite: Adjust tiling block sizes to BI=32 and BJ=128 (from current 64 and 256) to better fit per-core L1 cache capacity and improve temporal reuse of B and C sub-blocks inside the k-loop. Keep all other code and computation order identical to preserve numeric fidelity.] |
| 6 | rewrite_source | 5.111x  [rewrite: Add k-loop tiling with BK=64 inside the existing i-j tiles. Keep the single acc_tile array and do not introduce extra accumulators; the tiling must not change the order of floating-point additions (k increments sequentially within each tile, then moves to the next k tile). Also insert '#pragma clang loop vectorize(enable)' immediately before the inner j_off loop to force vectoriz |
| 7 | rewrite_source | 5.501x  [rewrite: Split the inner k-loop's j_off loop into two consecutive j_off loops: the first updates C[k][jj+j_off] using alphaB_tile[j_off] * aik, the second accumulates acc_tile[j_off] += B[k][jj+j_off] * aik. Then add '#pragma clang loop vectorize(enable)' immediately before each of the two new j_off loops to force SIMD vectorization. Keep all other code (tiling, alphaB_tile, acc_tile init |
| 8 | rewrite_source | 4.512x  [rewrite: For each of the two j_off loops inside the k-loop, insert '#pragma clang loop vectorize(enable) vectorize_width(8)' and '#pragma clang loop interleave(enable) interleave_count(2)' immediately before the loop to force 512-bit SIMD and 2× vector interleaving, while preserving the existing computation order.] |
| 9 | rewrite_source | 5.426x  [rewrite: 在当前最优内核的两个 for (int j_off = 0; j_off < j_len; j_off++) 循环（分别更新 C[k][jj+j_off] 和累加 acc_tile）之前，各添加 '#pragma clang loop unroll(enable)'，同时保留原有的 '#pragma clang loop vectorize(enable)'。其他代码（分块尺寸、临时数组、最终 C[i] 更新）保持不变。] |

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>3.9869x</b>（基线 1119.17 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb007`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：4.0431x　最终确认：**3.9869x**　IQR [3.8924, 4.0291]　base_cv=0.7% best_cv=1.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 3.043x  [rewrite: Interchange the j and k loops so that k becomes the innermost loop. This changes C[i][j] += ... to be computed over k inside the inner loop, requiring a temporary accumulator or a separate reduction step, but most importantly it makes the accesses A[j][k] and B[j][k] become A[j][k] (still strided) – wait, careful: after interchange, the loops become (i, j, k) where j is outer and |
| 2 | rewrite_source | 3.057x  [rewrite: 对 i 循环做 cache blocking，将外层 i 循环按块大小 BI（如 256）切分为多个块。每个 i 块开始时，将该块内所有 i 对应的 B[i][k]（k=0..M-1）加载到局部临时缓冲区 buffer_B[BI][M]。随后在原 j/k 循环中将所有 B[i][k] 引用替换为 buffer_B[i-i_start][k]，利用缓冲区的高局部性减少对主存的重复读取。C[i][j] 的累加仍在寄存器中进行（已由 acc 实现），不改变 FP 归约顺序，保证数值一致。] |
| 3 | rewrite_source | 2.907x  [rewrite: 在现有 i 循环分块（块大小 BI）基础上，对 j 循环也进行分块（块大小 BJ，如 128）。对于每个 i 块，先按 j 块加载 A[j][k] 和 B[j][k] 的行到两个局部二维缓冲区 bufA[BJ][M] 和 bufB[BJ][M]（仅加载 j_block 范围内的行）。然后内层 i_inner 循环仍预取 a_i/b_i，j_inner 循环遍历当前 j 块内所有 j <= i，k 循环使用 bufA[j_jj_start][k] 和 bufB[j_jj_start][k] 以及 a_i/b_i 进行累加。保留标量累加器 acc 和原有 FP 顺序，c 写回不变。] |
| 4 | rewrite_source | 3.642x  [rewrite: 在现有 i-loop blocking 和 loop interchange (j 外层、k 最内层) 基础上，对最内层 k 循环再进行一层 tiling：将 k 循环按块大小 BK（如 256）分块，外层按 BK 步进，内层在块内做原累加。保持每个 j 的累加器 acc 不变，k 整体递增顺序不变，数值完全一致。此变换使每个 k 块内的 A[j][k] 和 B[j][k] 连续区间驻留缓存，被同一 i 块内的多个 j 和 i 复用，且无需显式缓冲，从而降低内存带宽需求并提升向量化效率。] |
| 5 | rewrite_source | 3.665x  [rewrite: 合并 beta 缩放与累加：移除 i 块开头的独立 C 缩放循环，改为在 j 循环内将 acc 初始化为 beta * C[i][j]，消除一次额外的 C 数组遍历。同时对最内层 k 循环进行手动展开（unroll 4），保持单一累加器，以促进编译器生成更高效的向量化代码并隐藏浮点延迟。] |
| 6 | rewrite_source | 2.977x  [rewrite: 将 j 循环 (for j=0..i) 从 kk 块循环内部提升到外部（即交换 j 和 kk 的顺序），然后对 j 循环按块 BJ（如 128）分块，使 kk 循环和内层 k 循环处于最内层。对于每个 j 块，A[j][k] 和 B[j][k] 的行可常驻缓存，被同一 i 块内所有 i 和所有 kk 块复用，减少主存访问。累加器 acc 在 j 块内首个 kk 块时初始化为 beta*C[i][j]，后续 kk 块直接累加，保持数值顺序不变。不引入额外缓冲区，仅通过循环结构调整改善局部性。] |
| 7 | rewrite_source | 4.043x  [rewrite: 在current_best基础上，将i循环分块尺寸BI从64调整为32，将k循环分块尺寸BK从256调整为128，以使A[j][k]和B[j][k]的数据块（约128个double）更好地适配L1缓存，降低TLB压力；同时将最内层k循环的展开因子从4增加到8，并引入两个独立的累加器（acc0, acc1）来隐藏浮点乘加延迟。保持原有浮点运算顺序和C[i][j]的初始缩放不变，严格维护数值一致性。] |
| 8 | rewrite_source | 3.449x  [rewrite: 将i分块尺寸BI从32减小到16，k分块尺寸BK从128减小到64，以使每块数据（A/B行子块）更好地适配L1d缓存（1.3 MiB）；最内层k循环改为根据固定BK完全展开，并使用4个独立的累加器（acc0-acc3）按原始k索引交替累积，最终顺序求和以保证数值一致；在每个k块开始前用__builtin_prefetch预取下一个k块的A[j][k]和B[j][k]行，隐藏访存延迟。] |
| 9 | rewrite_source | 2.899x  [rewrite: 将独立的 C 缩放循环（for i ... for j ... C[i][j] *= beta）移除，改为在 j 循环内将累加器 acc 初始化为 beta * C[i][j]，直接进行累加。保持原循环顺序、i/k 分块尺寸、双累加器展开策略和所有浮点运算顺序不变。] |

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>2.5108x</b>（基线 354.80 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb008`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：2.7897x　最终确认：**2.5108x**　IQR [2.4934, 2.5274]　base_cv=0.9% best_cv=0.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.040x  [rewrite: 交换k与j循环的顺序：将k循环移至最内层，j循环移至外层，使A[i][k]和A[j][k]访问均连续；并将原有的C[i][j]*=beta循环融合到更新步骤，形成对每个j计算完整dot product后再写入C[i][j]的结构。] |
| 2 | rewrite_source | 2.790x  [rewrite: 对i循环和j循环添加cache tiling：将i、j各按32×32的tile分块（外层新增ii/jj循环，内层保持原始i/j/k顺序），保持每个C[i][j]的完整dot product不变。分块使A[j][*]在一个ii块内被多个i复用，同时A[i][*]也可在jj范围内复用，大幅提高L1/L2缓存命中，且数值完全一致。] |
| 3 | rewrite_source | 0.955x  [rewrite: Introduce an outer kk-loop that tiles the k (M) dimension with a block size BK (e.g., 128 or 256 determined later by the implementation LLM based on cache sizes). Within each kk-tile we keep the existing ii/jj i/j tiling and the inner i,j,k loops, so that for a given kk-block only a narrow column slice of A is accessed, enabling those columns to stay in L1/L2 across i and j itera |
| 4 | rewrite_source | 1.057x  [rewrite: 将 tile 大小 BI 和 BJ 从 32 调整为 128，保持现有的 i/j tiling 结构和循环顺序不变，数值计算完全一致。利用更大的 L1 cache 减少外层循环开销，并增加 A[j][*] 在 i 方向上的数据复用距离。] |
| 5 | rewrite_source | 0.999x  [rewrite: 将 tile 大小从 BI=32, BJ=32 调整为 BI=64, BJ=32，保持现有的外层 ii/jj 循环、内层 i/j/k 循环顺序以及所有浮点运算顺序完全不变，数值结果与原始代码一致。] |
| 6 | rewrite_source | 1.072x  [rewrite: 移除中间数组 tmp，在 ii/jj 分块内部，对每个 (i,j) 使用标量 accum 变量 sum 从 0 开始累加 alpha*A[i][k]*A[j][k]（保持 k 循环顺序不变），累加完成后执行 sum += beta*C[i][j]，再将 sum 写回 C[i][j]。该变换保持浮点运算顺序与原始完全一致，但消除了 tmp 数组的写入与后续读取，减少内存流量并提高寄存器利用率。] |
| 7 | rewrite_source | 0.961x  [rewrite: 在现有的 ii/jj 分块内，对 k 循环实施寄存器分块（micro-kernel）：将 k 维度分成 BK（如 64 或 128）的小段，在每个 kk 段内，手动展开 i 和 j 循环覆盖一个小的寄存器 tile（例如 RK=4, CJ=4），使用局部累加器数组（如 DATA_TYPE accum[RK][CJ] 初始化为 0）在 kk 段上累积 alpha*A[i][k]*A[j][k]；在每个 kk 段结束后，将 accum 加回 tmp 数组或 C 数组。此变换保持浮点累加顺序与原始一致，但通过寄存器级别的重用提高计算密度，减少访存，并使编译器更容易生成面向 SIMD 的向量乘加指令。] |
| 8 | rewrite_source | 1.043x  [rewrite: 移除中间数组 tmp：在 ii/jj 分块内部，对每个 (i,j) 使用标量累加器 sum 初始化为 beta*C[i][j]，然后在 k 循环内执行 sum += alpha*A[i][k]*A[j][k]（沿用现有 i/j/k 循环顺序），最后将 sum 写回 C[i][j]。保持 i/j tiling 32×32 不变，浮点运算顺序与原始源码完全一致，确保数值一致。] |
| 9 | rewrite_source | 0.949x  [rewrite: 在kernel_syrk的参数声明中，为指针参数C和A添加__restrict限定符（即 DATA_TYPE POLYBENCH_2D(C,N,N,n,n) 改为 const restrict 类型，A类似），并在函数体内适当使用const指针局部变量以进一步提示编译器无别名。保持所有循环结构、tile尺寸、tmp数组完全不变。此改动仅影响编译器别名分析，不改变任何运算顺序或内存访问模式，数值结果与原始完全一致。] |

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>1.0000x</b>（基线 1.87 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb017`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0326x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.590x  [rewrite(utils/adpcm_coder): Convert conditional branches in adpcm_coder's main loop (absolute value, sign, and clamping) to branchless arithmetic using bitwise and shift operations, aiming to reduce branch mispredictions and enable better ILP/vectorization.] |
| 2 | rewrite_source | 0.996x  [rewrite(utils/adpcm_coder): 手动将 adpcm_coder 主循环展开 2 次：每次处理两个连续输入样本，交替进行 valpred 更新与 delta 计算，并在展开体内合并 bufferstep 状态切换（两次迭代恰好输出一个字节），消除循环内 bufferstep 条件分支并降低循环计数开销。] |
| 3 | rewrite_source | 0.876x  [rewrite(utils/adpcm_coder): 在 adpcm_coder 函数参数中将 short indata[] 改为 short * restrict indata，char outdata[] 改为 char * restrict outdata，消除指针别名歧义，使编译器能够更激进地调度加载和存储操作。] |
| 4 | rewrite_source | 0.998x  [rewrite(utils/adpcm_coder): 在 adpcm_coder 主循环（for ( ; len > 0 ; len-- )）之前插入 #pragma clang loop unroll(enable) unroll_count(4)，建议编译器以 4 为因子展开循环，不修改循环体内的任何原有代码，保持所有依赖关系，避免引入额外开销。] |
| 5 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=6227febad457, opt=6025e67c9bba) |
| 6 | rewrite_source | 1.003x  [rewrite(utils/adpcm_coder): 手动将 adpcm_coder 的主循环展开4次，重新排列指令顺序，将每次迭代的 valpred 更新和钳位与下一次迭代的 diff 计算交错调度，以提高指令级并行度；同时正确处理 bufferstep 切换，确保每两次迭代输出一个字节，维持原始输出顺序。] |
| 7 | rewrite_source | 0.913x  [rewrite(utils/adpcm_coder): 将主循环（含剩余样本处理部分）中 valpred 的饱和钳位分支（if (valpred > 32767) ... else if (valpred < -32768) ...）替换为嵌套条件表达式：valpred = (valpred > 32767) ? 32767 : (valpred < -32768) ? -32768 : valpred; 其他代码完全不变，确保程序语义等价且不影响输出正确性。] |
| 8 | rewrite_source | 1.019x  [rewrite(utils/adpcm_coder): 将 adpcm_coder 的主循环拆分为两个独立循环：第一个循环遍历所有样本，仅计算每个样本的 delta 值（包括 sign 合并）并保存到局部临时数组（例如 short delta_vals[len]），不执行任何输出操作；第二个循环依据 bufferstep 初始状态，顺序读取 delta_vals 数组并与 outputbuffer 组合写入 outdata，并正确处理最后半个字节的残留。此变换分离计算和输出，减少第一个循环内部的指针递增、条件判断和存储指令，有望提高计算部分的指令级并行度和缓存局部性。] |
| 9 | rewrite_source | 1.033x  [rewrite(utils/adpcm_coder): 在 adpcm_coder 函数的第一个循环（计算 delta 并存入 delta_vals）中，手动展开 2 次迭代：每次处理两个连续输入样本，交替进行 valpred 更新、diff 计算和 delta 计算，并正确维护 index/step 状态，保持与原始串行语义等价；第二个打包循环不变。] |

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>1.0000x</b>（基线 1.46 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c1_cb018`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.8368x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 0.703x  [rewrite(utils/adpcm_decoder): 将 for ( ; len > 0 ; len-- ) 循环手动展开为每次处理 2 个输出样本（即一个输入字节的两个 nibble），移除 bufferstep 变量及其 if-else 分支，直接顺序提取高半字节与低半字节分别解码，循环计数减半（len>0 改为 len>=2，再处理可能的奇数次剩余样本）。此举可在不改变串行依赖的前提下消除分支与 toggle，降低循环开销并提升指令级并行。] |
| 2 | rewrite_source | 0.792x  [rewrite(utils/adpcm_decoder): Revert adpcm_decoder to its original bufferstep-driven loop (undo previous unrolling). Then insert #pragma clang loop unroll(enable) directly before the for loop to allow the compiler to auto‑unroll and schedule the original branchy loop body, leveraging its backend knowledge of the hardware.] |
| 3 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=f14432f8dd7b, opt=f89573bb1b00) |
| 4 | rewrite_source | 0.799x  [rewrite(utils/adpcm_decoder): 将 adpcm_decoder 的 for 循环手动复制为每次迭代处理 4 个 nibble（即两个输入字节），保留 bufferstep 标志及其 if-else 控制流，不消除分支。在展开的 4 个解码步骤之间直接使用原始序列（读取字节、提取 nibble、更新状态），以减少循环计数与跳转次数，同时让编译器在展开体内有机会覆盖 bufferstep 的交替预测并改善指令调度。所有中间状态严格按原始顺序更新，不与原始语义冲突。]content empty, falling back to reasoning_content on attempt 1 |
| 5 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=f14432f8dd7b, opt=f89573bb1b00) |
| 6 | rewrite_source | 1.837x  [rewrite(utils/adpcm_decoder): 将 adpcm_decoder 函数参数声明为 restrict 指针（char * restrict indata, short * restrict outdata, struct adpcm_state * restrict state）。重构 vpdiff 计算为无分支形式：vpdiff = (step>>3) + ((delta>>2)&1)*step + ((delta>>1)&1)*(step>>1) + (delta&1)*(step>>2)。保持其他逻辑完全不变。] |
| 7 | rewrite_source | 0.977x  [rewrite(utils/adpcm_decoder): Inside adpcm_decoder, replace the global stepsizeTable and indexTable accesses with two static const local arrays whose contents are hard‑coded copies of the original tables (int stepsizeTable[89] = {…}; int indexTable[16] = {…};). Keep all other logic (restrict pointers, branchless vpdiff) unchanged.] |
| 8 | rewrite_source | 1.090x  [rewrite(utils/adpcm_decoder): Rewrite the index clamping to a single branchless expression: replace `if ( index < 0 ) index = 0; if ( index > 88 ) index = 88;` with `index = (index < 0) ? 0 : ((index > 88) ? 88 : index);`. Leave all other logic (restrict pointers, branchless vpdiff, bufferstep, etc.) unchanged.] |
| 9 | rewrite_source | 1.015x  [rewrite(utils/adpcm_decoder): Add __attribute__((always_inline)) before the definition of adpcm_decoder in utils/polybench.c, keeping all other logic (restrict, branchless vpdiff, bufferstep, clamping) unchanged. This forces the compiler to inline the function into kernel_telecom_adpcm_d's loop, eliminating call/return overhead and enabling better register allocation across the call bound |

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>0.9971x</b>（基线 0.93 ms，hash 校验，23/51 次为正）</summary>

- 任务 `c1_cb019`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0932x　最终确认：**0.9971x**　IQR [0.9495, 1.0116]　base_cv=29.9% best_cv=30.9%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 失败 [rewrite_source] optimized version returned non-zero exit code -11 |
| 2 | rewrite_source | 0.969x  [rewrite: 对 crc32file 的主处理循环再次实施 slicing-by-4 变换（先保守使用 4 路以避免过度复杂）：将单一 crc 累加器替换为 4 个独立累加器 crc0..crc3，并预计算 4 张 CRC 查找表（table0 即原表，table1..table3 基于原表递推生成）。主循环每次读取 4 字节，分别使用 crcX = tableX[(crcX ^ byteX) & 0xFF] 更新，并确保仅当剩余字节数 >= 4 时才按 4 字节块处理；退出循环后用逐字节方式处理剩余 0-3 字节。最后通过常规公式组合 crc0..crc3 得到最终 CRC。所有缓冲区访问必须严格检查边界，避免越界读取导致段错误。] |
| 3 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpdyzn68cz/telecom_crc32_rewrite.c:129:32: error: character <U+FF0C> not allowed in an identifier |
| 4 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpb8pt7_qs/telecom_crc32_rewrite.c:129:1: error: unknown type name '我们被要求实现' |
| 5 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpryhy67my/telecom_crc32_rewrite.c:157:1: error: expected '}' |
| 6 | rewrite_source | 0.783x  [rewrite: 对 crc32file 的主循环进行手动循环展开（展开因子 4），将逐字节处理改为每次迭代连续处理 4 个字节，但保持单一的 CRC 累加器（不分割为多路）。同时将缓冲区指针和 CRC 查找表用 restrict/const 限定，帮助编译器消除别名歧义。循环剩余 0–3 字节仍用逐字节循环处理。变换仅减少循环分支开销和存储访问次数，不改变数据依赖链，期望通过更好的指令调度和更少的循环开销获得微小加速。] |
| 7 | rewrite_source | 0.580x  [rewrite: 在 crc32file 的主循环中用 x86 SSE4.2 CRC32 硬件指令（__builtin_ia32_crc32qi / _mm_crc32_u8 等）逐字节更新 crc，替代软件查表。使用 #ifdef __SSE4_2__ 保护，若未定义则回退原始实现。该变换不改变 CRC 算法语义，仅替换底层运算单元，预期大幅降低单字节更新延迟。] |
| 8 | rewrite_source | 1.093x  [rewrite: 将 crc32file 的逐字节主循环改为每次处理4字节的 word‑at‑a‑time 算法：预计算4张CRC查找表（table0 为原表，table1/2/3 根据递推生成），主循环条件为剩余长度≥4，每次用 uint32_t 指针读取4字节数据，然后通过 crc = table0[crc>>24] ^ table1[(crc>>16)&0xFF] ^ table2[(crc>>8)&0xFF] ^ table3[crc&0xFF] 更新单个 crc 累加器，完全避免多累加器组合步骤；剩余0‑3字节仍用原逐字节查表处理。确保所有边界检查正确，表定义为 static const 并在首次调用时初始化一次。] |
| 9 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): output hash mismatch (ref=7e61aed697f6, opt=da96890fba5d) |

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>0.9802x</b>（基线 9.28 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c1_pb021`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.2759x　最终确认：**0.9802x**　IQR [0.9392, 1.0996]　base_cv=9.0% best_cv=6.9%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 1.007x  [rewrite: 将内层循环中的x[i]累加提升为局部标量变量s，初始化s=b[i]，在j循环中更新s -= L[i][j] * x[j]，循环结束后x[i] = s / L[i][i]。消除对x[i]的多次写入。] |
| 2 | rewrite_source | 1.001x  [rewrite: 对 kernel_trisolv 的外层 i 循环进行分块（cache blocking），块大小取 B=64 或 128。将原单个 i 循环改写为双层循环：外层遍历块起始 ii (ii+=B)，内层在块内仍按 i 从 ii 到 min(ii+B-1,N-1) 的顺序计算 x[i]；对于每个 i，其点积累加不改变顺序（先累加所有 j<ii 的部分，再累加块内 j 从 ii 到 i-1 的部分），以保证浮点结果与原始一致。同时，块内所有 i 共享已计算的 x[0..i-1]，使得 x 数组的块内段被频繁重用，从而提升缓存命中率并降低平均访存延迟。] |
| 3 | rewrite_source | 0.967x  [rewrite: 在kernel_trisolv的内层循环for(j=0;j<i;j++)内部，在每次计算s -= L[i][j]*x[j]之前，提前插入__builtin_prefetch指令预取x[j+128]和L[i][j+128]（若j+128<i），以提升缓存命中率、隐藏访存延迟。保持所有计算顺序和数值不变。]content empty, falling back to reasoning_content on attempt 1 |
| 4 | rewrite_source | 1.230x  [rewrite: 将外层i循环以块大小B=64分块。对每个块ii，分配局部临时数组x_tile[B]（可使用DATA_TYPE x_tile[B]或alloca）。对于块内每个i（从ii到min(ii+B-1, N-1)），计算s = b[i]后，先用j循环累加j<ii的部分（使用全局x[j]和L[i][j]），再用j循环累加块内部分j从ii到i-1（使用x_tile[j-ii]和L[i][j]）。计算完x[i]后存入x_tile[i-ii]而非全局x[i]。块内所有行处理完后，将x_tile中的值一次写回全局x[ii..i_end]。该变换消除块内x值的写后读延迟，提升缓存局部性。] |
| 5 | rewrite_source | 1.071x  [rewrite: Take the current best kernel (tiled with local x_tile and scalar accumulator). Add __restrict to all array parameters (L, x, b) to disambiguate pointers. Insert #pragma clang loop vectorize(enable) vectorize_width(4) immediately before each inner j-for-loop (the one for j<ii and the one for ii<=j<i) to force 256-bit SIMD reduction of s. Finally, change TILE_SIZE from 64 to 128 to |
| 6 | rewrite_source | 1.027x  [rewrite: 在当前最佳kernel（使用x_tile和标量累加器）基础上，将TILE_SIZE增加到96或128以更好利用L2缓存并减少tile切换次数；并将x_tile声明为alignas(64) DATA_TYPE x_tile[TILE_SIZE]以对齐缓存行，提高编译器自动向量化效率。不添加任何向量化pragma，保留标量累加顺序。] |
| 7 | rewrite_source | 1.276x  [rewrite: 在 kernel_trisolv 的分块循环中，对于每个 i，先计算 inv_diag = 1.0 / L[i][i]，然后将 x_tile[i - ii] = s / L[i][i] 替换为 x_tile[i - ii] = s * inv_diag。其他所有循环结构、访问顺序和标量累加保持不变。] |
| 8 | rewrite_source | 1.123x  [rewrite: 在 current_best 基础上做两处修改：(1) 将 TILE_SIZE 从 64 改为 32，减小块尺寸以缓解 L 矩阵行遍历与局部 x_tile 对 L1 缓存的竞争；(2) 为函数参数 L、x、b 添加 __restrict 类型限定符，以告知编译器数组不重叠，帮助消除别名分析障碍并可能改善内层循环的加载/存储调度。保持标量累加器和倒数乘法不变。] |
| 9 | rewrite_source | 0.940x  [rewrite: 在 current_best 的基础上，添加 __restrict 到所有数组参数 (L, x, b)，并手工展开内层两个 j 循环（j<ii 和 j 在 [ii,i)）各 4 次，保持标量累加顺序不变，以增加指令级并行和减少循环开销。] |

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>6.6763x</b>（基线 525.61 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c1_pb009`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：8.2966x　最终确认：**6.6763x**　IQR [6.5870, 6.6802]　base_cv=0.2% best_cv=0.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | rewrite_source | 7.517x  [rewrite: 将 for-j 和 for-k 循环交换，使最内层变为 j-loop；对每个固定的 i 和 k，将 A[k][i] 提升为标量寄存器变量，在内层连续遍历 j 完成 B[i][j] += sc*A * B[k][j] 的累积；将乘法 alpha 移到 j 循环之后，单独用一个 j 循环执行 B[i][j] = alpha * B[i][j]，以保持语义且消除内循环的常数乘法开销。] |
| 2 | rewrite_source | 6.012x  [rewrite: Apply loop tiling (cache blocking) to the i and k loops, with a tile size of TI=64 and TK=64 (or similar tuned sizes). Within a tile, the innermost loop remains j for contiguous access. Specifically: for (int ii = 0; ii < _PB_M; ii += TI) { for (int kk = ii+1; kk < _PB_M; kk += TK) { for (int i = ii; i < min(ii+TI, _PB_M); i++) { for (int k = max(i+1, kk); k < min(kk+TK, _PB_M);  |
| 3 | rewrite_source | 7.934x  [rewrite: Apply loop tiling to the i and k loops with tile sizes TI=16 and TK=16 (or similarly small values). Inside the tile, preserve the innermost j-loop for contiguous memory access. Before the j-loop add pragma directives: '#pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)' to force 256-bit SIMD vectorization and moderate unrolling. Keep th |
| 4 | rewrite_source | 8.276x  [rewrite: 在 i 和 k 的 tile 循环中将块大小改为 TI=32, TK=32；在最内层 j 循环前插入 '#pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4)'，强制生成 512-bit SIMD 指令并深度展开；保持外层循环顺序及 alpha 缩放不变。] |
| 5 | rewrite_source | 7.758x  [rewrite: 交换 tile 循环顺序：将外层 for (int ii = 0; ii < _PB_M; ii += TI) 和内层 for (int kk = ii+1; kk < _PB_M; kk += TK) 对调，改为 for (int kk = 0; kk < _PB_M; kk += TK) 外层、for (int ii = 0; ii < kk; ii += TI) 内层，并调整三角边界使 k-start = max(ii, kk, i+1)。保留最内层 j 循环的 #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4) 以保证 512‑bit SIMD 与深度展开。] |
| 6 | rewrite_source | 7.462x  [rewrite: Add a per-i scalar accumulator array: for each i inside the i-tile, first load B[i][j] into a local double acc[_PB_N] for j=0.._PB_N-1. Then in the k-tile loop, for each k, compute A[k][i]*B[k][j] and accumulate into acc[j]. After the k-tile loop, write back alpha*acc[j] to B[i][j], eliminating the separate alpha loop. Keep TI=32; increase TK to 64 or 96 to amortise k-loop overhe |
| 7 | rewrite_source | 8.297x  [rewrite: Modify tile sizes to TI=16, TK=64. Keep the outer ii loop advancing by TI=16, inner kk loop advancing by TK=64. Inside the i-loop, for each i, load A[k][i] as a scalar; inside the j-loop, apply '#pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4) unroll(enable) unroll_count(2)' to force 512-bit SIMD, depth-4 interleaving, and a factor-2  |
| 8 | rewrite_source | 5.573x  [rewrite: Modify tile sizes to TI=8, TK=128. Keep all other loop structure and pragma directives identical to current best (ii loop outer, kk loop inner, triangular bounds, #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4) unroll(enable) unroll_count(2) on the innermost j-loop, and separate alpha scaling loop after the k-tile). The smaller i-til |
| 9 | rewrite_source | 4.773x  [rewrite: 保持 ii 外层、kk 内层循环结构及 TI=16, TK=64 与三角边界不变。仅修改最内层 j 循环前的 pragma，将 unroll 计数从 2 提升至 4，同时将 interleave 计数从 4 降至 2，即使用 '#pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2) unroll(enable) unroll_count(4)'，其他所有内容（包括单独的 alpha 缩放循环）保持不变。该调整旨在通过更高程度的展开增加指令级并行度，同时降低交织度避免寄存器压力过大。] |

</details>

### ② no-compiler-feedback（自由选动作，屏蔽编译器反馈）（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>5.4382x</b>（基线 1072.18 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb010`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：5.6790x　最终确认：**5.4382x**　IQR [5.4123, 5.4802]　base_cv=0.2% best_cv=0.6%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 5.038x  [rewrite: 对两个矩阵乘法循环分别进行循环交换：将内层的 k 循环与中层的 j 循环对调，使 j 成为最内层，从而让 B[k][j] 和 C[k][j] 变为连续访问（B[k][j] 中 j 连续），同时提升 tmp[i][j] 和 D[i][j] 的连续性。保持 i 最外层不变。] |
| 3 | try_flags | 5.075x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 5.646x  [rewrite: 对两个矩阵乘法循环实施 i 和 k 的二维 cache blocking：在外层添加 ii, kk 循环分别以块大小 BI, BK 遍历 NI, NK，内部保持原有 i,k,j 顺序；同时在内层 j 前引入局部数组 scalar_acc[NJ] 初始化为零，累加 alpha*A*B 后写回 tmp（或 D），以减少内存 store-load 并且促进向量化。对最内层 j 循环添加 #pragma clang loop vectorize(enable) 以强制向量化。] |
| 6 | try_flags | 5.312x  [flags: -mllvm -vectorizer-min-trip-count=1] |
| 7 | try_pragma | 5.287x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4); #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(4)] |
| 8 | rewrite_source | 5.679x  [rewrite: 针对两个矩阵乘法循环的最内层 j 循环，手动进行 4 次展开（strip-mining）：将循环步长改为 4，内部使用 4 个独立的标量累加器（acc0, acc1, acc2, acc3）分别对应 j, j+1, j+2, j+3，最后将累加器结果写回原数组。同时保留已有的 i/k 分块和标量累加外层。此变换可提高指令级并行度，减少内存访问事务。] |
| 9 | try_flags | 5.463x  [flags: -mllvm -slp-threshold=2 -mllvm -unroll-threshold=1000] |

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>4.8067x</b>（基线 1594.94 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb011`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：5.5404x　最终确认：**4.8067x**　IQR [4.1083, 4.9239]　base_cv=11.8% best_cv=2.4%
- 最终采纳编译选项：`-mllvm -inline-threshold=225`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 4.154x  [rewrite: Interchange the j and k loops in each of the three matrix-multiply nests (E:=A*B, F:=C*D, G:=E*F) to make the j loop innermost. That is, transform for(i) for(j) { init; for(k) update; } into for(i) { for(j) init; for(k) for(j) update; }. This ensures arrays B, D, and F are accessed contiguously as B[k][j] with j varying fastest, improving cache locality and enabling auto-vectoriz |
| 3 | try_flags | 4.631x  [flags: -mllvm -inline-threshold=225] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 失败 [rewrite_source] 第二层验证失败: [STANDARD_DATASET] optimized version returned non-zero exit code -11 |
| 6 | try_flags | 3.993x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 7 | try_pragma | 5.091x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(4); #pragma clang loop vectorize(enable) vectorize_width(4)] |
| 8 | rewrite_source | 5.540x  [rewrite: In each of the three matrix-multiply nests (E:=A*B, F:=C*D, G:=E*F), before the innermost j loop, load A[i][k] (resp. C[i][k], E[i][k]) into a local scalar variable lik = A[i][k] and replace the array reference inside the j loop with lik. This eliminates redundant loads caused by potential aliasing, preserves the exact same summation order, and is safe for correctness.] |
| 9 | try_flags | 4.709x  [flags: -mllvm -slp-threshold=-10] |

</details>

<details><summary><b>adi</b> — 中位加速比 <b>1.5090x</b>（基线 6600.52 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb025`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.4993x　最终确认：**1.5090x**　IQR [1.5086, 1.5098]　base_cv=0.0% best_cv=0.0%
- 最终采纳编译选项：`-mllvm -slp-min-tree-size=3`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.008x  [flags: -mllvm -inline-threshold=1800] |
| 2 | rewrite_source | 1.469x  [rewrite: 对列扫描部分实施循环交换：将 for (i=1; i<_PB_N-1; i++) 内的两个 j 循环（向前扫描与向后扫描）与外层 i 循环互换。具体做法：向前扫描先遍历 j=1..N-2，内层遍历 i=1..N-2，计算 p[i][j] 和 q[i][j]（仅依赖同一 i 的 j-1 步值，无跨 i 依赖）；向后扫描以 j=N-2..1 倒序为外层，i 内层，计算 v[j][i]。这样每次固定的 j 会使所有 i 的 u[j][i] 访问变成同一行的连续内存，消除步长 N 的缓存污染。行扫描已经具有连续访存特性，保持不变。] |
| 3 | try_flags | 1.490x  [flags: -mllvm -inline-threshold=3500] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.116x  [rewrite: 对列扫描的前向求解循环（j 在外，i 在内）进行 i 维度分块：外层 for (i_blk=1; i_blk<_PB_N-1; i_blk+=BK)，内层保持 for (j=1; j<_PB_N-1; j++)，最内层 for (i=i_blk; i<min(i_blk+BK,_PB_N-1); i++)，使每一块内的所有行在遍历 j 时被反复使用，改善 u 的缓存局部性和 p/q 的空间局部性。逆向求解循环同理分块。同时标量化分母 denom = a*p[i][j-1]+b，分别用于 p 和 q 的计算，消除冗余浮点操作。块大小 BK 由实现 LLM 根据 L1 缓存容量选择。] |
| 6 | try_flags | 1.499x  [flags: -mllvm -slp-min-tree-size=3] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 1.473x  [rewrite: 对行扫描（Row Sweep）的 i 循环进行缓存分块（tiling）。将外层 i 循环（i=1..N-2）按块大小 BK=128 或 256 分成多个块，每个块内仍保持原始的 i 外、j 前向/后向内层嵌套，不改动 j 的遍历方向与依赖顺序。块间顺序执行，保持全局语义。同时在 p[i][j] 和 q[i][j] 计算中将公共分母 d*p[i][j-1]+e 和分子中 v 相关部分提取到局部变量，消除冗余浮点操作。此变换使得 v 数组的相邻若干行（约 BK+2 行）在 L2 中被重用，减少容量失效，又因为保持 i 外 j 内结构，u 的写入和 v 的访问仍保持连续。] |
| 9 | try_flags | 1.452x  [flags: -mllvm -inline-threshold=3000] |

</details>

<details><summary><b>atax</b> — 中位加速比 <b>0.9687x</b>（基线 22.20 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c2_pb012`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0673x　最终确认：**0.9687x**　IQR [0.9327, 1.0388]　base_cv=3.7% best_cv=1.8%
- 最终采纳编译选项：`-mllvm -inline-threshold=900`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.042x  [flags: -mllvm -inline-threshold=900] |
| 2 | rewrite_source | 0.995x  [rewrite: Manually unroll both inner j-loops by a factor of 4 (with a clean-up epilogue) while preserving the original sequential floating-point accumulation order: for the first loop, accumulate tmp[i] using a scalar variable in four additive steps per unrolled iteration; for the second loop, update y[j+0..3] sequentially. This avoids changing the arithmetic order and complies with numeri |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.037x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 5 | rewrite_source | 1.067x  [rewrite: Apply i-loop tiling: split the outer i-loop into blocks of size B (e.g., 32 or 64). Inside each block, first compute tmp[i] for all i in the block using the first inner j-loop, then update y[j] for all j using the second inner j-loop across the whole block. This reduces the number of times the full y array is written from M to ceil(M/B), improving cache reuse and reducing memory  |
| 6 | try_flags | 1.017x  [flags: -mllvm -unroll-partial-threshold=800] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.025x  [rewrite: 在现有i-loop tiling的基础上，对tmp[i]的计算部分进行循环交换：将j循环提升到i循环外部，并使用标量变量xj=x[j]来避免重复加载x数组。保持y更新循环不变（其按行访问已对缓存友好）。这样，对于每个j，x[j]只加载一次，供块内所有i共享，减少内存带宽消耗。浮点累加顺序与原代码完全一致，保证数值无差异。] |
| 9 | rewrite_source | 1.022x  [rewrite: 在现有的 i-loop tiling 代码中，为每个块内的 tmp[i] 计算循环引入一个标量累加器 `DATA_TYPE sum = 0.0`，将内层 j 循环中的 `tmp[i] += A[i][j] * x[j]` 替换为 `sum += A[i][j] * x[j]`，循环结束后再赋值 `tmp[i] = sum`。这完全保持原有累加顺序，消除对 tmp[i] 地址的冗余读写，帮助编译器生成更高效的归约向量代码。] |

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>0.9995x</b>（基线 14.59 ms，hash 校验，18/37 次为正，⚠ 正确性门无效）</summary>

- 任务 `c2_cb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0531x　最终确认：**0.9995x**　IQR [0.9952, 1.0079]　base_cv=2.9% best_cv=3.1%
- 最终采纳编译选项：`-mllvm -unroll-threshold=500`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.036x  [rewrite(utils/qsortx): Inline the implementations of swap() and shortsort() directly into qsortx. Replace every swap(a,b,width) call with an explicit for‑loop that exchanges bytes between a and b, and replace the shortsort(lo,hi,width,comp) call with an inlined insertion‑sort implementation using the same comp function. This removes two crossing‑module calls inside the hot partitioning an |
| 3 | try_flags | 1.053x  [flags: -mllvm -unroll-threshold=500] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 0.997x  [rewrite(utils/qsortx): 重写插入排序子循环：将当前元素拷贝到临时栈数组 char tmp[width]，然后用 __builtin_memmove 将前一个元素整体后移一个元素宽度，最后用 __builtin_memcpy 将 tmp 插入空位；分区中的元素交换也用 __builtin_memcpy 配合 char tmp[width] 完成。这消除逐字节循环，利用编译器内建 memmove/memcpy 生成 SIMD 或展开的块拷贝。] |
| 6 | rewrite_source | 0.995x  [rewrite(utils/qsortx): 在 qsortx 内现有的逐字节元素交换循环（swap 块和插入排序移动循环）中，根据 width 的值特化拷贝：若 width == 4，使用 *(int32_t*) 直接赋值；若 width == 8，使用 *(int64_t*) 赋值；其他宽度回退到原有逐字节循环。这消除对小固定宽度元素的循环开销，生成高效的内联 mov 指令。] |
| 7 | try_pragma | 1.025x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.009x  [rewrite(utils/qsortx): 在 qsortx 内部，对于元素宽度 width 为 4 或 8 的常见情况，直接通过 *(int32_t*) 或 *(int64_t*) 解引用并进行整数比较（使用 <、>、<=、>=），完全绕过函数指针 comp；其他宽度回退到原来的 comp 调用。这消除了间接调用开销和函数调用栈帧，使编译器能够生成高效的条件移动指令或内联比较。] |
| 9 | try_flags | 1.014x  [flags: -mllvm -slp-threshold=-10 -mllvm -unroll-allow-partial=1 -mllvm -inline-threshold=600] |

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>1.0053x</b>（基线 5.19 ms，hash 校验，28/51 次为正）</summary>

- 任务 `c2_cb002`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0691x　最终确认：**1.0053x**　IQR [0.9538, 1.0269]　base_cv=6.6% best_cv=7.1%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=16`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.069x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 2 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpxf5djlvw/automotive_susan_corners_rewrite.c:1749:1: error: type specifier missing, defaults to 'int'; ISO C99 and later do not support implicit int [-Wimplicit-int] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp4ivifamr/automotive_susan_corners_rewrite.c:1752:1: error: function definition is not allowed here |
| 5 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpc01y9ez_/automotive_susan_corners_rewrite.c:1454:1: error: unknown type name 'We' |
| 8 | try_flags | 1.057x  [flags: -mllvm -inline-threshold=1000] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>1.0153x</b>（基线 7.95 ms，hash 校验，48/51 次为正）</summary>

- 任务 `c2_cb003`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0888x　最终确认：**1.0153x**　IQR [1.0117, 1.0194]　base_cv=1.2% best_cv=0.9%
- 最终采纳编译选项：`-mllvm -slp-threshold=10 -mllvm -inlinehint-threshold=700`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.027x  [flags: -mllvm -slp-threshold=10 -mllvm -inlinehint-threshold=700] |
| 2 | rewrite_source | 0.995x  [rewrite: Apply 2D loop tiling (cache blocking) to the main nested loop that scans the image (likely outer loop over rows 'i' and inner loop over columns 'j') performing 3×3 neighbor comparisons. Introduce two enclosing tile loops (ii=0; ii<rows; ii+=64, jj=0; jj<cols; jj+=64) and adjust the original 'i','j' loops to run within [ii, min(ii+64,rows)] and [jj, min(jj+64,cols)]. Use the exist |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.010x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 5 | rewrite_source | 1.023x  [rewrite: Fully unroll the innermost nested loops that iterate over the 3×3 neighbour offsets (e.g., for dx=-1..1 and dy=-1..1) by hand. Replace the loop with 9 explicit comparisons/accumulations using local scalar variables, preserving the original reduction logic exactly. This eliminates loop counter updates and branch overhead, exposes more ILP, and allows the compiler to schedule the n |
| 6 | try_flags | 1.014x  [flags: -mllvm -inlinehint-threshold=1000] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.089x  [rewrite: 在 `susan_thin` 函数中扫描所有像素的最外层 for‑for 嵌套循环（例如 `for (int i=1; i<rows-1; ++i)` 及内层 `for (int j=1; j<cols-1; ++j)`）之前插入 `#pragma clang loop vectorize(enable)`。保持循环体不变，该循环体对每个像素独立计算中心与 3×3 邻域的比较和条件写回，无循环携带依赖，可以安全向量化。同时继续使用当前最优 flags (`-slp-threshold=10 -inlinehint-threshold=700`) 以保留已有的内联和 SLP 收益。] |
| 9 | try_flags | 1.006x  [flags: -mllvm -slp-threshold=-1] |

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.1170x</b>（基线 61.94 ms，hash 校验，9/9 次为正）</summary>

- 任务 `c2_cb004`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0906x　最终确认：**1.1170x**　IQR [1.1145, 1.1182]　base_cv=0.1% best_cv=0.2%
- 最终采纳编译选项：`-mllvm -small-loop-cost=8`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.002x  [rewrite: Apply loop tiling to the main smoothing loop nest: tile both the row and column loops with a block size of 32x32, introduce a temporary scalar accumulator for each output pixel within the inner tile, and hoist invariant mask indices outside the inner loops. This increases L1 cache reuse of image data and reduces memory bandwidth pressure.] |
| 3 | try_flags | 1.004x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 4 | try_pragma | 0.951x  [pragma: #pragma clang loop vectorize(enable) interleave(enable)] |
| 5 | rewrite_source | 1.004x  [rewrite: Unroll the innermost loop over x in the large-mask Gaussian smoothing section with factor 2. Load two consecutive brightness values per iteration, compute two independent 'tmp' products, and accumulate them into 'area' and 'total' sequentially (preserving original order to guarantee numerical consistency). Handle the remaining odd element after the loop separately. This reduces b |
| 6 | try_flags | 1.056x  [flags: -mllvm -small-loop-cost=8] |
| 7 | try_pragma | 0.897x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.091x  [rewrite: Unroll the innermost x-loop in the large-mask Gaussian smoothing with factor 4: load four consecutive brightness values upfront, compute four independent tmp products using four consecutive dp weights, and accumulate them sequentially into area and total. Handle remaining tail iterations (<4) in a separate clean-up loop.] |

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>1.6535x</b>（基线 27.06 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb013`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：2.2623x　最终确认：**1.6535x**　IQR [1.5927, 1.7205]　base_cv=1.5% best_cv=3.8%
- 最终采纳编译选项：`-mllvm -unroll-threshold=100`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.055x  [flags: -mllvm -unroll-threshold=100] |
| 2 | rewrite_source | 2.262x  [rewrite: 对外层循环 for (i = 0; i < _PB_N; i++) 进行 cache tiling，分块大小 B 选取 32（或根据 L1 大小自适应，如 B = 128/sizeof(DATA_TYPE)），重组为双层循环：外层 tile i 循环，内层 tile j 循环，以保证 s 向量完整停留在 L1 缓存中。同时将 q[i] 的归约改为局部标量累加器 q_i，在内层 j 循环中累加，循环结束后一次性写回 q[i]，减少重复的 store-load 操作。] |
| 3 | try_flags | 1.000x  [flags: -mllvm -unroll-threshold=100] |
| 4 | try_pragma | 1.811x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2); #pragma clang loop vectorize(enable) vectorize_width(8)] |
| 5 | rewrite_source | 1.866x  [rewrite: 在现有 j 分块（B_J=32）基础上增加对 i 循环的二维分块（B_I=64）：外层循环遍历 i 块，中层遍历 j 块，内层保持原始 i、j 循环顺序。保持 q[i] 的局部标量累加器和 s[j] 的原地累加。] |
| 6 | try_flags | 1.927x  [flags: -mllvm -slp-max-reg-size=512] |
| 7 | rewrite_source | 1.216x  [rewrite: 将 jj 分块大小常量 B 从 32 改为 128（128*sizeof(DATA_TYPE)=1KB，远小于 L1d 容量 32KB），使 s 向量一个完整的 tile 常驻 L1 缓存，同时大幅减少外层 jj 循环的迭代次数，降低循环开销并潜在改善编译器展开与向量化决策。其他结构（循环顺序、q[i] 标量累加器）保持不变。] |
| 8 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 9 | try_flags | 1.713x  [flags: -mllvm -licm-max-num-uses-traversed=32] |

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>1.0394x</b>（基线 58.37 ms，hash 校验，6/9 次为正）</summary>

- 任务 `c2_cb005`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：3.6485x　最终确认：**1.0394x**　IQR [0.9393, 1.1033]　base_cv=11.8% best_cv=8.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 3.648x  [rewrite: Merge the three loops that iterate over the argList linked list into a single loop. During one traversal, compute the longest filename, process single-character flags, and process long flags. This eliminates redundant pointer dereferences and repeated string comparisons, reducing execution time.] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 0.998x  [rewrite: In the single merged loop over argList, replace the strcmp(aa->name, "--") check with a direct character comparison (aa->name[0]=='-' && aa->name[1]=='-' && aa->name[2]=='\0'), and cache the result of strlen(aa->name) in a local Int32 variable to avoid calling strlen twice per filename when computing the longest name.] |

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>1.0109x</b>（基线 94.06 ms，hash 校验，7/7 次为正）</summary>

- 任务 `c2_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1066x　最终确认：**1.0109x**　IQR [1.0036, 1.0114]　base_cv=0.1% best_cv=0.3%
- 最终采纳编译选项：`-mllvm -unroll-threshold=900`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.010x  [flags: -mllvm -gvn-max-block-speculations=800] |
| 2 | rewrite_source | 0.998x  [rewrite: 在计算最长文件名的循环中，将 strlen(aa->name) 的结果缓存到局部变量 len 中，以避免重复调用 strlen。同时，将 progName 路径分隔符的手动循环替换为 strrchr 调用，直接定位最后一个 PATH_SEP。] |
| 4 | rewrite_source | 1.098x  [flags: -mllvm -jump-threading-threshold=48] |
| 5 | try_flags | 1.006x  [rewrite: Merge the two flag‑parsing loops (the single‑character short‑flag loop and the long‑flag if‑else chain) into a single traversal of argList. Within the merged loop, first handle short flags when the argument name starts with a single '-' (excluding '--'), then handle long flags when the name starts with '--', and break when the '--' delimiter is encountered. This saves one complet |
| 6 | try_pragma | 1.104x  [flags: -mllvm -unroll-threshold=900] |
| 7 | rewrite_source | 1.003x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | try_flags | 1.001x  [rewrite: 将 argList 链表的多次遍历改为数组顺序访问：在计算 numFileNames 和最长文件名的第一个循环中，将每一个有效文件名节点的指针存入栈上的 Cell* 数组（假设最大参数个数足够），后续的所有 flag 解析、压缩/解压循环都改为遍历该数组，避免指针追逐造成的 cache miss 和间接跳转。] |
| 9 |  | 1.107x  [flags: -mllvm -unroll-threshold=900] |

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0038x</b>（基线 27137.68 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb016`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0248x　最终确认：**1.0038x**　IQR [1.0015, 1.0218]　base_cv=0.9% best_cv=1.0%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.001x  [flags: -mllvm -slp-max-reg-size=256] |
| 2 | rewrite_source | 1.023x  [rewrite: Introduce a scalar accumulator in the j<i loop: load A[i][j] into a local variable tmp before the k-loop, accumulate over k into tmp without writing back to A[i][j], and after the k-loop perform A[i][j] = tmp / A[j][j]. This eliminates redundant stores to A[i][j], reduces memory traffic, and may permit the compiler to vectorize the inner k-loop more aggressively.] |
| 3 | try_flags | 1.011x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 4 | rewrite_source | 0.995x  [rewrite: Implement cache blocking (tiling) on the outer i-loop with tile size B=64. For each block, first compute the Cholesky decomposition of the diagonal block (same algorithm but restricted to the block rows/columns), then perform a blocked triangular solve and symmetric rank-k update for the remaining rows below the block. Use a local scratch buffer (double tile_buf[B*B]) to load a s |
| 5 | try_pragma | 0.999x  [pragma: #pragma clang loop vectorize(enable) interleave(enable)] |
| 6 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): [SMALL_DATASET] Numeric mismatch: max relative error 1.00e-02 at index 433 (ref=0.77, opt=0.78), epsilon=1.00e-04 |
| 7 | try_flags | 1.025x  [flags: -mllvm -slp-threshold=-4] |
| 8 | try_pragma | 1.000x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.024x  [flags: -mllvm -slp-threshold=-8] |

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>1.0000x</b>（基线 1.52 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c2_cb007`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1381x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.003x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 开头将 td 指针的常用成员（td_rowsperstrip, td_stripsperimage, td_imagelength, td_planarconfig, td_samplesperpixel）加载到局部 const/register 变量，并替换函数内所有对 td->... 的访问为这些局部变量，以减少间接 load 开销。] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 0.957x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.002x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 中将 tif 的常用成员（tif->tif_curstrip, tif->tif_row, tif->tif_flags）提升为局部变量，并在关键条件判断（如 strip != tif->tif_curstrip）上使用 __builtin_expect 指示常见路径，以减少重复解引用并改善分支预测。] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 1.138x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 中，将 strip 与 tif->tif_curstrip 的比较前置，并将仅与条带切换相关的检查（如 strip >= nstrips 扩展条带、编解码器设置等）移入该分支内，使得同一 strip 连续写入的常见路径能快速跳过这些检查，直抵 encoderow 调用。] |
| 8 | try_pragma | 0.996x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | rewrite_source | 0.998x  [rewrite(utils/TIFFWriteScanline): 在 TIFFWriteScanline 中，将 row 与 tif->tif_row 的比较前置于 strip 计算之前，假设 row==tif->tif_row 为常见路径（使用 __builtin_expect），仅当 row 不相等时再计算 strip 并执行条带切换与 seek，以减少连续写入快速路径的指令数。] |

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>1.0005x</b>（基线 2.19 ms，hash 校验，28/51 次为正）</summary>

- 任务 `c2_cb008`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1627x　最终确认：**1.0005x**　IQR [0.9968, 1.0113]　base_cv=4.1% best_cv=3.6%
- 最终采纳编译选项：`-mllvm -vectorize-memory-check-threshold=1024`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.011x  [flags: -mllvm -vectorize-memory-check-threshold=1024] |
| 2 | rewrite_source | 1.062x  [rewrite(utils/TIFFWriteDirectory): Cache frequently accessed tif members (tif_flags, tif_mode, tif_dir pointer) into local variables at function entry to minimize pointer chasing; add __builtin_expect(..., 0) to the O_RDONLY early return, the TIFF_SWAB byte-swap loop, and all goto bad error paths to improve branch prediction for the common fast path; manually unroll the 'for (b = 0; b <=  |
| 3 | try_flags | 1.043x  [flags: -mllvm -unroll-threshold=100] |
| 4 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpaw4x7bs2/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 5 | try_pragma | 1.163x  [pragma: #pragma clang loop vectorize(enable)] |
| 6 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=1024] |
| 7 | rewrite_source | 0.975x  [rewrite(utils/TIFFWriteDirectory): In the TIFF_SWAB branch, replace the manual shift-based byte-swapping of each TIFFDirEntry field (tdir_tag, tdir_type, tdir_count, tdir_offset) with __builtin_bswap16 and __builtin_bswap32; likewise use __builtin_bswap16/32 for the final dircount and diroff swaps. Unroll the swap loop by 2 (process two entries per iteration) to expose instruction-level p |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | try_flags | 1.031x  [flags: -mllvm -vectorize-memory-check-threshold=4096] |

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>1.0000x</b>（基线 0.88 ms，hash 校验，0/0 次为正，⚠ 正确性门无效）</summary>

- 任务 `c2_cb009`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0477x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.048x  [rewrite(utils/TIFFReadDirectory): 将第一个目录遍历循环（for (dp = dir, n = dircount; n > 0; n--, dp++)）内部的 if (tif->tif_flags & TIFF_SWAB) 判断提升到循环外部，通过 if-else 产生两个版本的循环体，从而消除每次迭代的条件跳转。] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | rewrite_source | 0.872x  [rewrite(utils/TIFFReadDirectory): 在第二个 pass 循环前，将 tif->tif_dir 的 td_bitspersample、td_samplesperpixel、td_nstrips 读出到局部 const 变量，并在循环体内部用这些局部变量替换对应的 td-> 间接访问；同时为所有 goto bad 路径和 MissingRequired 调用添加 __builtin_expect(..., 0) 将冷分支告知编译器。] |
| 5 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 6 | rewrite_source | 0.888x  [rewrite(utils/TIFFReadDirectory): 在第二个循环（第一个真实 pass）前，将 tif->tif_fieldinfo 和 tif->tif_nfields 复制到局部 const 指针/整数，并在循环体内用局部变量替换所有 tif->tif_fieldinfo 和 tif->tif_nfields 的访问。第三个循环（第二个 pass）也同样提升，消除冗余的通过 tif 指针的间接内存读取。] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | rewrite_source | 0.912x  [rewrite(utils/TIFFReadDirectory): 在第二个循环（第一个真实 pass）前，提取 tif->tif_fieldinfo 中各项的 field_tag 到本地数组 field_tags，保持升序；循环内将原有的 while (fix < tif->tif_nfields && tif->tif_fieldinfo[fix]->field_tag < dp->tdir_tag) fix++ 替换为基于 field_tags 的二分查找定位 correct fieldinfo 索引，保留未找到时的忽略和错误提示逻辑，但避免线性扫描造成的 O(n²) 退化。] |

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>1.0005x</b>（基线 3.26 ms，hash 校验，26/51 次为正）</summary>

- 任务 `c2_cb010`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1508x　最终确认：**1.0005x**　IQR [0.9903, 1.0139]　base_cv=3.9% best_cv=3.8%
- 最终采纳编译选项：`-mllvm -vectorizer-min-trip-count=2`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.151x  [flags: -mllvm -vectorizer-min-trip-count=2] |
| 2 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpwegfp087/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | rewrite_source | 1.012x  [rewrite(utils/TIFFWriteDirectory): 将 TIFF_SWAB 条件下的指针递增循环改写为基于索引的 for 循环，内联字节交换操作（直接使用位移和掩码实现 TIFF_SwabShort/Long 逻辑），并添加 #pragma clang loop vectorize(enable) interleave(enable) 提示。同时保证 dircount 减速和后续写入正确，避免触发编译器警告（尤其注意括号和类型转换）。] |
| 5 | try_flags | 1.136x  [flags: -mllvm -vectorizer-min-trip-count=2] |
| 6 | try_pragma | 0.973x  [pragma: #pragma clang loop vectorize(enable)] |
| 7 | rewrite_source | 0.995x  [rewrite(utils/TIFFWriteDirectory): 在 TIFFWriteDirectory 中将 _TIFFmalloc(dirsize) 替换为栈分配优先策略：定义一个固定大小的栈数组 char data_buf[4096]; char *data = data_buf; 当 dirsize > 4096 时才调用 _TIFFmalloc；在 bad 标签与正常出口处，仅当 data != data_buf 时释放 data；其余代码不变。这样可消除绝大多数目录写入时的 malloc/free 开销。] |
| 8 | try_flags | 1.150x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 9 | try_flags | 1.089x  [flags: -mllvm -licm-max-num-uses-traversed=32 -mllvm -vectorizer-min-trip-count=2] |

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>18.8261x</b>（基线 4891.07 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c2_pb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：18.9543x　最终确认：**18.8261x**　IQR [18.8149, 18.8439]　base_cv=0.2% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -slp-vectorize-hor=0`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 13.553x  [rewrite: Interchange loops in the correlation computation: hoist the k loop (over N) to become the outermost, so that for each k we load one row of data and accumulate into all corr[i][j] entries. This transforms the data access pattern from strided column-wise (data[k][i]) to contiguous row-wise (data[k][i] with adjacent k now consecutive), drastically improving cache reuse and vectoriz |
| 3 | try_flags | 13.729x  [flags: -mllvm -unroll-allow-partial=1 -mllvm -slp-max-reg-size=256 -mllvm -inline-threshold=225] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 13.504x  [rewrite: 在外部 k 循环内，预先将 data[k] 整行复制到局部数组 DATA_TYPE data_row[M]，并用 const DATA_TYPE * restrict row = data_row 传递给内层循环。同时将 corr[i][j] 的累加改为通过 restrict 指针 corr_row = corr[i] 访问，消除 anyiasing 歧义，帮助编译器向量化内层 j 循环。] |
| 6 | try_flags | 13.631x  [flags: -mllvm -loop-flatten-cost-threshold=8] |
| 7 | rewrite_source | 18.831x  [rewrite: 对 k 循环内的 i 和 j 索引进行 tiling：引入分块大小 B_I 和 B_J (例如 32 或 64)，将循环重排为 for ib in i 块, for jb in j 块, for k, for i in ib 块, for j in jb 块。这样每个 (i,j) 块内的 corr 更新在遍历 k 时始终驻留在 cache 中，大幅提高 corr 矩阵的缓存局部性。分块内循环保持向量化友好，且不改变浮点累加顺序，不影响数值结果。] |
| 8 | try_pragma | 18.023x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 18.954x  [flags: -mllvm -slp-vectorize-hor=0] |

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>14.4213x</b>（基线 1478.58 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb002`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：17.4164x　最终确认：**14.4213x**　IQR [14.1584, 15.0710]　base_cv=0.4% best_cv=3.2%
- 最终采纳编译选项：`-mllvm -unroll-threshold=600`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.017x  [flags: -mllvm -unroll-threshold=600] |
| 2 | rewrite_source | 9.916x  [rewrite: Interchange the triply nested loops in the covariance section: move the innermost k loop (over N) to the outermost position, producing order: for k, for i, for j (j from i to M-1). This makes all accesses to data[k][...] contiguous (unit‑stride) and enables efficient cache reuse and vectorization. The reduction order is re‑associated but the final cov[i][j] values remain numerica |
| 3 | try_flags | 8.530x  [flags: -mllvm -slp-min-tree-size=5] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 4.711x  [rewrite: 对最外层的 i 循环实施 cache blocking（tiling），块大小为 64。具体地，引入块循环：for (int ii = 0; ii < _PB_M; ii += 64)，在块内执行原有的 k 和 i,j 累加循环，限制 i 在 [ii, min(ii+64, _PB_M)) 范围内，j 仍然从 i 开始。这样 cov[i][j] 的更新被限制在一个行块内，可在 L1/L2 中复用，减少内存写入次数。保持最终除以 (float_n-1) 和对称赋值不变。] |
| 6 | try_flags | 9.908x  [flags: -mllvm -unroll-threshold=2000 -mllvm -slp-min-tree-size=4 -mllvm -unroll-partial-threshold=150] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 17.416x  [rewrite: 对最外层k循环进行4倍展开（带尾部处理）：每次迭代同时累加k, k+1, k+2, k+3的贡献到cov[i][j]，即将内层j循环的内容改为 cov[i][j] += d_k0_i*data[k][j] + d_k1_i*data[k+1][j] + d_k2_i*data[k+2][j] + d_k3_i*data[k+3][j]，从而4次外积更新只对cov[i][j]进行一次读写，大幅降低内存带宽需求，同时保留内层j循环的连续访存和自动向量化能力。] |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>1.5369x</b>（基线 141.68 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb022`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.6412x　最终确认：**1.5369x**　IQR [1.4576, 1.5640]　base_cv=0.9% best_cv=2.8%
- 最终采纳编译选项：`-mllvm -gvn-max-block-speculations=9600`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.071x  [flags: -mllvm -gvn-max-block-speculations=600] |
| 2 | rewrite_source | 1.559x  [rewrite: 交换垂直前向和垂直后向循环的遍历顺序（外层 i for 0..w-1，内层 j for 0..h-1），将递推状态变量 tm1, ym1, ym2, tp1, tp2, yp1, yp2 改为大小为 h 的标量数组，使按行连续访问 imgOut 以改善 L1 cache 局部性；水平方向保持不变。] |
| 3 | try_flags | 1.581x  [flags: -mllvm -gvn-max-block-speculations=4800] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.523x  [rewrite: 将垂直滤波阶段的三个双重循环（合并循环、前向垂直滤波循环、反向垂直滤波循环）融合为两个外循环：在i递增外循环内部，先计算 imgOut[i][j] = y1[i][j] + y2[i][j] （即合并），然后立即进行前向垂直滤波更新 y1[i][j]；在i递减外循环内部，先进行反向垂直滤波更新 y2[i][j]，然后立即计算最终 imgOut[i][j] = y1[i][j] + y2[i][j]。这样可以消除对 imgOut 和 y1/y2 的冗余存取，提高数据重用。] |
| 6 | try_flags | 1.641x  [flags: -mllvm -gvn-max-block-speculations=9600] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 1.474x  [rewrite: Apply loop strip-mining (cache blocking) on the j dimension of the vertical forward and backward passes: split the inner for (j=0; j<_PB_H; j++) into an outer for (jj=0; jj<_PB_H; jj+=BS) and inner for (j=jj; j<min(jj+BS,_PB_H); j++) with a block size BS=256. This keeps a slice of the state arrays (tm1_arr, ym1_arr, ym2_arr, etc.) and image rows in L1 cache across the i loop, imp |
| 9 | try_flags | 1.448x  [flags: -mllvm -slp-threshold=-1] |

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>3.8695x</b>（基线 246.75 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb014`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：4.1024x　最终确认：**3.8695x**　IQR [3.6456, 4.0125]　base_cv=1.5% best_cv=6.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.040x  [flags: -mllvm -small-loop-cost=5] |
| 2 | rewrite_source | 3.998x  [rewrite: Loop interchange: move the s-loop outside the p-loop so that the innermost loop is over p, accessing C4[s][p] contiguously and accumulating sum[p] with vectorizable multiply-add. This transforms the original inner two loops from 'for p { sum[p]=0; for s sum[p]+=A[r][q][s]*C4[s][p]; }' to 'for p sum[p]=0; for s { a=A[r][q][s]; for p sum[p]+=a*C4[s][p]; }'.  Also keep the final wri |
| 3 | try_flags | 4.102x  [flags: -mllvm -unroll-allow-partial=1] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 3.967x  [rewrite: 在 r 和 q 循环外增加双层 tile 循环，将 r 和 q 分成 B_R x B_Q 的小块。对每个 tile，先清零 sum 数组，然后遍历 s，对于 tile 内的所有 (r,q) 计算 A[r][q][s] 与 C4[s][p] 的乘加并更新各自的 sum，最后将 tile 内的 sum 写回 A[r][q][p]。这确保同一个 C4[s][p] 行被 tile 内多个 (r,q) 重复使用，降低缓存冷失。] |
| 6 | try_flags | 4.081x  [flags: -mllvm -slp-threshold=5] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 4.093x  [rewrite: 在 (r,q) 循环内部，对 p 循环进行 cache blocking（sum tiling）。将原 NP 维分成大小为 B_P（如 64）的小块；对每个 tile，先清零该 tile 的 sum 片段，遍历所有 s 完成乘加累加，再将该 tile 写回 A[r][q][p]。这样 sum 的每个小块可以驻留在寄存器或 L1，减少整体内存带宽消耗。保持循环交换的结构不变，不改变累加顺序。] |
| 9 | try_flags | 3.690x  [flags: -mllvm -licm-max-num-uses-traversed=16] |

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.5689x</b>（基线 3.32 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb017`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.6946x　最终确认：**1.5689x**　IQR [1.5408, 1.5706]　base_cv=3.6% best_cv=2.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.215x  [rewrite: 消除临时数组 z，将两个 for 循环 (z[i]=... 和 y[i]=z[i]) 合并为一个反向循环 (i 从 k-1 到 0)，直接原地更新 y[i] = y[i] + alpha*y[k-i-1]，减少内存带宽。] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 1.550x  [pragma: #pragma clang loop vectorize(enable); #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 0.995x  [rewrite: 将 y 更新循环拆分为两部分：首先用一个无分支的循环处理 i < k/2 的对称位置更新（i != j），然后单独处理当 k 为奇数时中间元素 i == j 的自更新（y[i] += alpha * y[i]）。这消除了原始循环每步的条件判断，使编译器能够生成更紧密的向量化代码。] |
| 6 | try_flags | 1.470x  [flags: -mllvm -unroll-threshold=300] |
| 7 | rewrite_source | 1.695x  [rewrite: 在函数参数声明中给 r 和 y 添加 restrict 关键字，声明两个数组不重叠，消除可能的别名分析失败，使编译器能安全地向量化 y 更新循环并重排内存操作。] |
| 8 | try_pragma | 失败 [try_pragma] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp6_y548_g/durbin_pragma.c:80:18: error: duplicate directives 'vectorize(enable)' and 'vectorize(enable)' |
| 9 | try_pragma | 1.604x  [pragma: #pragma clang loop interleave(enable) interleave_count(2)] |

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>0.9989x</b>（基线 750.00 ms，numeric 校验，0/3 次为正）</summary>

- 任务 `c2_pb026`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.5908x　最终确认：**0.9989x**　IQR [0.9934, 0.9989]　base_cv=16.7% best_cv=16.4%
- 最终采纳编译选项：`-mllvm -slp-threshold=-20`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.011x  [flags: -mllvm -slp-threshold=-16] |
| 2 | rewrite_source | 1.009x  [rewrite: Swap the i and j loops in the hz update (j outer, i inner) to make memory accesses more contiguous, and apply cache blocking (tile size 64) to the i-dimension of the ey update loop to increase L1 data reuse.] |
| 3 | try_flags | 1.472x  [flags: -mllvm -slp-vectorize-hor=1 -mllvm -inline-threshold=400 -mllvm -licm-max-num-uses-traversed=32] |
| 4 | try_pragma | 0.993x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2); #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)] |
| 5 | rewrite_source | 0.912x  [rewrite: 对 ey、ex、hz 三个空间更新循环分别应用二维 tiling（cache blocking），tile size 建议 32 或 64（根据 L1d cache 容量），保持原始循环顺序和边界偏移（ey 从 i=1 开始，ex 从 j=1 开始，hz 在 i<NX-1,j<NY-1）。在时间步循环内，对每个更新循环单独做分块遍历，以提升空间局部性并减少主存访问。] |
| 6 | try_flags | 1.591x  [flags: -mllvm -slp-threshold=-20] |
| 7 | rewrite_source | 1.117x  [rewrite: 融合 ey 和 ex 更新循环：将原代码中各自独立的 ey 更新循环（i=1..NX-1, j=0..NY-1）和 ex 更新循环（i=0..NX-1, j=1..NY-1）合并为单个嵌套循环，在交集区域 i=1..NX-1, j=1..NY-1 内同时计算 ey[i][j] 和 ex[i][j]，以重用对 hz 的读取；余下边界条件（ey 的 j=0 列、ex 的 i=0 行）单独处理，保持数值等价。] |
| 8 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 9 | try_flags | 1.300x  [flags: -mllvm -unroll-threshold=1000] |

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>2.9166x</b>（基线 15852.91 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c2_pb023`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：2.9189x　最终确认：**2.9166x**　IQR [2.9022, 2.9196]　base_cv=0.0% best_cv=0.3%
- 最终采纳编译选项：`-mllvm -unroll-threshold=1500 -mllvm -vectorizer-min-trip-count=1 -mllvm -inline-threshold=225`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.077x  [rewrite: Tile the i and j loops with block sizes II_BLOCK=64 and JJ_BLOCK=64 (tunable) for each fixed k. For each k, iterate over ii blocks of i, then over jj blocks of j, and compute the inner updates for ii..ii+B-1 and jj..jj+B-1. This promotes reuse of the path[i][k] column block and the path[k][j] row block in L1 cache.] |
| 3 | try_flags | 1.150x  [flags: -mllvm -unroll-threshold=400] |
| 4 | try_pragma | 1.075x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 2.024x  [rewrite: In the tiled loops, for each i hoist path[i][k] into a local scalar aik, and replace path[i][k] with aik inside the innermost j-loop. This removes repeated non-contiguous columnar loads and enables the compiler to vectorize a simpler min-add-select on two contiguous arrays.] |
| 6 | try_flags | 2.640x  [flags: -mllvm -unroll-threshold=1500 -mllvm -vectorizer-min-trip-count=1 -mllvm -inline-threshold=225] |
| 7 | rewrite_source | 2.919x  [rewrite: 在每个k循环迭代开始时，创建一个大小为_N的一维局部数组row_k，将path[k][0.._N-1]复制进去。然后在ii/jj/i/j的循环体内，所有引用path[k][j]的地方替换为row_k[j]。这样将二维数组的重复访问转化为一维数组的直接访问，消除别名分析负担和数组索引计算，使内层循环的向量化更加高效。] |
| 8 | try_pragma | 2.908x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(8) interleave(enable) interleave_count(2)] |
| 9 | try_flags | 2.911x  [flags: -mllvm -unroll-threshold=3000] |

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>1.0000x</b>（基线 279.95 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c2_pb003`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.294x  [rewrite: 交换 j 和 k 循环：将 `for (k) for (j)` 改为 `for (j) for (k)`，并在每次 i 迭代内、j 循环之前用标量变量 `sum` 初始化 `C[i][j]*beta`，然后在最内层 k 循环中累加 `alpha * A[i][k] * B[k][j]`，最后写回 `C[i][j] = sum`。这样可以消除内层对 C 的重复写入。] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | rewrite_source | 0.978x  [rewrite: 对 i 和 k 循环进行 cache blocking: 将 for(i) 循环用 BI 分块，将 for(k) 循环用 BK 分块，保持 j 循环最内层不变，以在 L1/L2 cache 中重用 A 的块和 B 的行段，同时保持 B 的连续访存模式。具体：外 ii 循环 (i 块)，内 i 循环缩放 C 行，再内 kk 循环 (k 块)，内 k 循环，最内 j 循环乘加。不交换 j 和 k。] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | rewrite_source | 0.855x  [rewrite: Introduce a temporary accumulator array for each i row to reduce writes to C: before the k loop, initialize a local array 'acc[NJ]' to zero. In the inner k loop, instead of updating C[i][j] directly, accumulate 'acc[j] += alpha * A[i][k] * B[k][j]' (still stride-1 on B). After the k loop completes, combine with beta in a single j-loop: 'C[i][j] = C[i][j] * beta + acc[j]'. The ori |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.5446x</b>（基线 35.66 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb004`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.5126x　最终确认：**1.5446x**　IQR [1.5324, 1.5535]　base_cv=1.0% best_cv=0.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.501x  [rewrite: 交换第二个双重循环的循环顺序：将外层 i、内层 j 改为外层 j、内层 i，使得矩阵 A 的访问模式由 A[j][i] 列访问变为按行连续访问，提升 cache 局部性并利于向量化。] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.513x  [rewrite: 对第四个双重循环（w[i] = w[i] + alpha * A[i][j] * x[j]）应用 loop tiling/cache blocking：将外层 i 和内层 j 分别分块，典型 tile size 取 64 或由编译器根据 SIZE 自适应，使得一个 tile 内的 A 和 x 完全驻留在 L2 cache 中，提升数据重用，减少内存带宽压力。] |
| 6 | try_flags | 1.450x  [flags: -mllvm -inline-threshold=500] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.413x  [rewrite: Apply loop tiling (cache blocking) to the second double loop (the x[i] reduction). Keep the already interchanged loop order (j outer, i inner), then tile the i dimension with a block size B=256. The resulting loop nest becomes: for (ii=0; ii<N; ii+=256) for (j=0; j<N; j++) for (i=ii; i<min(ii+256,N); i++) x[i] += beta * A[j][i] * y[j]. This reuses each block of x across all j, dr |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>1.3057x</b>（基线 23.11 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb005`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.3175x　最终确认：**1.3057x**　IQR [1.2755, 1.3254]　base_cv=2.6% best_cv=3.1%
- 最终采纳编译选项：`-mllvm -unroll-allow-partial=1`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.045x  [flags: -mllvm -vectorizer-min-trip-count=2] |
| 2 | rewrite_source | 1.023x  [rewrite: 对原始二重循环进行loop tiling：将外层i循环以TI=32为块大小分块，将内层j循环以TJ=256为块大小分块，形成四层循环遍历ii, jj, i, j，在内层保持原始tmp[i]和y[i]的累加计算不变。该变换不改变每个固定i的j累加顺序，因此数值结果完全一致，同时能让x[jj..jj+TJ-1]片段在L1缓存中被同一i块内的所有i迭代重复使用，大幅降低x的内存加载次数，并改善A、B的行访问局部性。] |
| 3 | try_flags | 1.000x  [flags: -mllvm -vectorizer-min-trip-count=2] |
| 4 | try_pragma | 0.988x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.291x  [rewrite: 使用 scalar accumulator 消除内层循环中的重复内存访问：在外层 i 循环内、内层 j 循环前声明局部变量 double sum_tmp = 0.0, sum_y = 0.0；内层循环中累加到 sum_tmp 和 sum_y，循环结束后赋值给 tmp[i] 和 y[i]，再执行 y[i] = alpha * tmp[i] + beta * y[i]。保持所有操作顺序不变以保证数值一致。] |
| 6 | try_flags | 1.317x  [flags: -mllvm -unroll-allow-partial=1] |
| 7 | rewrite_source | 1.290x  [rewrite: 对当前标量累加器版本的j循环进行手动循环展开4次（unroll factor 4），每次迭代计算4个连续的j索引，保持单一累加器顺序（每次累加A[i][j+k]*x[j+k]到sum_tmp和B[i][j+k]*x[j+k]到sum_y，k=0..3，依次进行），剩余尾部循环单独处理。这样确保数值顺序与原始一致。] |
| 8 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.68e-04 at index 74 (ref=59.48, opt=59.47), epsilon=1.00e-04 |
| 9 | try_flags | 1.301x  [flags: -mllvm -unroll-threshold=9600] |

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>5.7986x</b>（基线 1509.83 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb018`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：7.0014x　最终确认：**5.7986x**　IQR [5.7351, 5.9879]　base_cv=1.2% best_cv=1.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 6.453x  [rewrite: 将 A 更新循环的 i 和 j 嵌套交换：原 for (j=k+1; j<_PB_N; j++) { for (i=0; i<_PB_M; i++) { A[i][j] = A[i][j] - Q[i][k]*R[k][j]; } } 改为 for (i=0; i<_PB_M; i++) { for (j=k+1; j<_PB_N; j++) { A[i][j] = A[i][j] - Q[i][k]*R[k][j]; } }，使内层 j 连续访问 A[i][j] 和 R[k][j] 而消除 stride-M 跨步。] |
| 3 | try_flags | 5.777x  [flags: -mllvm -slp-vectorize-hor=1 -mllvm -prefetch-distance=512 -mllvm -licm-max-num-uses-traversed=64] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 5.865x  [rewrite: 对 A 更新循环（当前为 for(i) { for(j) { A[i][j] -= qik * R[k][j]; } }）引入 2D 缓存分块：将 i 和 j 分别划分为块大小 Ti、Tj（例如 64），使外层按 j 块循环，中间按 i 块循环，内层执行 tile 内的 i 和 j 遍历，让 R[k][j] 的一个块在 i 块内被重复使用，减少 R 向量的重复加载，提高缓存命中率。] |
| 6 | try_flags | 6.571x  [flags: -mllvm -slp-min-tree-size=1] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 7.001x  [rewrite: 合并 R[k][j] 的归零和点积累加循环：删除单独的 R[k][j]=0 循环，将 i 的第一次迭代(i=0)用于初始化 R[k][j]=Q[0][k]*A[0][j]，然后从 i=1 开始累加，保持外层 i、内层 j 的循环顺序以维持 A[i][j] 连续访问。这消除了冗余的 j 维度一遍遍历，减少内存写入压力。] |
| 9 | try_flags | 6.555x  [flags: -mllvm -unroll-threshold=600] |

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.0574x</b>（基线 2311.15 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c2_pb027`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0734x　最终确认：**1.0574x**　IQR [1.0565, 1.0580]　base_cv=0.0% best_cv=0.0%
- 最终采纳编译选项：`-mllvm -slp-threshold=0`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.002x  [flags: -mllvm -slp-threshold=0] |
| 2 | rewrite_source | 1.073x  [rewrite: Introduce spatial tiling on the i and j loops inside each time step. Wrap the existing i loop in an outer tile_i loop and the j loop in an outer tile_j loop (e.g., tile sizes 32×32). For each tile, compute B and then A only over the tile region, so that the active working set of A and B remains in L1/L2 cache before moving to the next tile. The innermost k loop is traversed fully |
| 3 | try_flags | 1.000x  [flags: -mllvm -slp-threshold=0] |
| 4 | rewrite_source | 1.066x  [rewrite: Add __restrict qualifiers to the function parameters A and B (i.e., declare them as DATA_TYPE (* __restrict A) or DATA_TYPE * __restrict A, depending on the actual array type) to tell the compiler that the arrays do not alias, enabling more efficient vectorization and instruction scheduling. Keep the existing spatial tiling (TILE_I=32, TILE_J=32) and all other logic unchanged.] |
| 5 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 6 | try_flags | 1.054x  [flags: -mllvm -slp-threshold=-1 -mllvm -unroll-threshold=150 -mllvm -jump-threading-threshold=6] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 0.972x  [rewrite: Add __restrict qualifiers to the function parameters A and B (i.e., declare them as DATA_TYPE (* __restrict A) or DATA_TYPE * __restrict A, depending on the actual array type) to tell the compiler that the arrays do not alias. Keep the existing spatial tiling (TILE_I=32, TILE_J=32) and all other logic unchanged.] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>1.0019x</b>（基线 2.53 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c2_pb028`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.6388x　最终确认：**1.0019x**　IQR [0.9940, 1.0432]　base_cv=1.3% best_cv=1.4%
- 最终采纳编译选项：`-mllvm -unroll-partial-threshold=2000`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.594x  [flags: -mllvm -unroll-threshold=1000] |
| 2 | rewrite_source | 1.550x  [rewrite: Apply 1D temporal blocking (time tiling) to the stencil: partition the tsteps loop into blocks of size T (e.g., 32), and within each time block, process the spatial domain in cache‑sized chunks. For each spatial chunk, perform all time‑block iterations using local arrays / sliding windows so that intermediate values stay in L1 cache, drastically reducing main‑memory traffic witho |
| 3 | try_flags | 1.000x  [flags: -mllvm -unroll-threshold=1000] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.408x  [rewrite: Add __restrict qualifiers to A and B parameters to eliminate possible aliasing, enabling the compiler to generate wider SIMD vectorization and more aggressive instruction scheduling for the 1D stencil.] |
| 6 | try_flags | 1.639x  [flags: -mllvm -unroll-partial-threshold=2000] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.235x  [rewrite: 手工展开计算B数组和更新A数组的两个内层循环，展开因子均为4。对计算B的循环，提前加载A[i-1],A[i],A[i+1],A[i+2],A[i+3],A[i+4]到局部变量a0..a5，然后计算B[i]到B[i+3]时重用a1,a2,a3等中间值，减少对A的重复读取。对更新A的循环做对称处理，使用B的局部变量。尾部用标量循环处理剩余迭代。] |
| 9 | try_flags | 1.000x  [flags: -mllvm -unroll-partial-threshold=2000] |

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>1.1933x</b>（基线 1123.69 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb029`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.1936x　最终确认：**1.1933x**　IQR [1.1929, 1.1948]　base_cv=0.1% best_cv=0.2%
- 最终采纳编译选项：`-mllvm -unroll-allow-partial=1 -mllvm -inline-threshold=800`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.001x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-reg-size=2048] |
| 2 | rewrite_source | 0.690x  [rewrite: Apply loop tiling (cache blocking) to the i,j loops inside each time step. Tile both the B update loop and the A update loop with a tile size of 32×32, using a local scalar temporary for the stencil in the A update to avoid redundant loads. The tiling ensures that a 32×32 block of A and B stays in L1 cache while the inner tile loops compute the stencil, drastically reducing cache |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.194x  [flags: -mllvm -unroll-allow-partial=1 -mllvm -inline-threshold=800] |
| 5 | rewrite_source | 1.018x  [rewrite: Apply sliding-window scalar replacement to the innermost j loops in both stencil updates. For the B update loop, maintain three scalar variables (a_prev, a_cur, a_next) holding A[i][j-1], A[i][j], and A[i][j+1] so that each iteration only loads A[i-1][j], A[i+1][j], and the next window element, reducing loads per iteration from 5 to 3. Apply the same windowing for the A update lo |
| 6 | try_flags | 1.192x  [flags: -mllvm -unroll-allow-partial=1 -mllvm -slp-min-reg-size=64 -mllvm -inline-threshold=2000] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.000x  [rewrite: Insert manual prefetch in both innermost j loops to hide memory latency: in the B-update loop, add __builtin_prefetch(&A[i+2][j], 0, 3) inside the j loop, and analogously prefetch &B[i+2][j] inside the A-update loop. Keep all other loop structures intact to preserve contiguous access and autovectorization.] |
| 9 | try_flags | 1.000x  [flags: -mllvm -unroll-allow-partial=1 -mllvm -inline-threshold=800] |

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.2438x</b>（基线 32724.41 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb020`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.2532x　最终确认：**1.2438x**　IQR [1.2204, 1.2560]　base_cv=1.3% best_cv=1.5%
- 最终采纳编译选项：`-mllvm -unroll-allow-partial=1`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.015x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 2 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): [SMALL_DATASET] Numeric mismatch: max relative error 1.00e-02 at index 483 (ref=0.98, opt=0.97), epsilon=1.00e-04 |
| 3 | try_pragma | 1.020x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | rewrite_source | 0.981x  [rewrite: 对 kernel_lu 中的两个 j 循环（j∈[0,i) 和 j∈[i,N)）按 tile size B=64 进行缓存分块（loop tiling）：在每个分块的 j 区域内部仍保持原始递增顺序，同时 k 循环完全保留在分块内部且按原始顺序遍历。该变换不重排任何浮点加法或乘法顺序，仅通过缩短时间上 j 维度的跨度来复用 A[k][j] 的列数据，从而在不牺牲数值一致性的前提下提升缓存命中率。同时加载当前最优的 pragma 向量化提示（#pragma clang loop vectorize(enable)）以及 -mllvm -licm-max-num-uses-traversed=64 标志以延续已有的收益。] |
| 5 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 6 | rewrite_source | 1.214x  [rewrite: 对 kernel_lu 的两个 j 循环（j∈[0,i) 和 j∈[i,N)）分别引入局部标量累加器 sum：将内层 k 循环中对 A[i][j] 的重复减乘操作替换为 sum += A[i][k] * A[k][j]，循环结束后再将 sum 施加到 A[i][j]（或先减 sum 再除以 A[j][j]）。此变换不改变访存序列，但把 O(i) 次写合并为一次写，暴露更干净的归约模式以便向量化，同时保留现有的 #pragma clang loop vectorize(enable) 和 LICM 标志。] |
| 7 | try_pragma | 失败 [try_pragma] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpy3omt4iu/lu_pragma.c:94:21: error: duplicate directives 'vectorize(enable)' and 'vectorize(enable)' |
| 8 | try_flags | 1.253x  [flags: -mllvm -unroll-allow-partial=1] |
| 9 | rewrite_source | 1.208x  [rewrite: 将除法阶段与更新阶段分离：先对所有k=0..i-1执行A[i][k]/=A[k][k]；然后对j循环从1到N-1进行缓存分块（块大小B=64），每一块内保持标量累加器sum，内层k循环累积A[i][k]*A[k][j]，块结束时一次性更新A[i][j]。该变换不改变浮点累加顺序，且通过j分块使A[k][j]列数据在块内被重用，提升缓存局部性。] |

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.0746x</b>（基线 7562.85 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb019`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.1682x　最终确认：**1.0746x**　IQR [1.0508, 1.0782]　base_cv=3.5% best_cv=3.6%
- 最终采纳编译选项：`-mllvm -unroll-threshold=300`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.835x  [rewrite: 在函数开头声明 double temp_col[N]（使用栈分配或参数传递的 POLYBENCH_1D），在前向消去循环（第一个内层循环 for j<i）中，对于每个 j，先将 A[0..j-1][j] 拷贝到 temp_col[0..j-1]（连续存储），然后在内层 k 循环中用 temp_col[k] 替换 A[k][j]。类似地，在第二个内层循环 for j>=i 中，对于每个 j，将 A[0..i-1][j] 拷贝到 temp_col，然后用 temp_col[k] 替换 A[k][j]。该变换仅将跨行访问转为连续访问，不改变浮点运算顺序，保证数值一致。] |
| 3 | try_pragma | 0.989x  [pragma: #pragma clang loop vectorize(enable) interleave(enable); #pragma clang loop vectorize(enable) interleave(enable)] |
| 4 | try_flags | 1.028x  [flags: -mllvm -unroll-threshold=300] |
| 5 | rewrite_source | 0.869x  [rewrite: 对两个j循环（j<i 和 j>=i）进行分块（block size B=16）：在i循环体内，将j循环划分为大小为B的块。对每个块[j_start, j_end)，在进入k循环之前，将A[0..j_end-1][j_start..j_end]的所有元素拷贝到临时二维数组temp[D1][D2]，按列存储（即temp[k][j-j_start]连续存放），后续内层k循环中使用temp[k][j-j_start]代替原A[k][j]访问。这样将跨行访问转为连续访问，且每块拷贝一次性完成，所有j在块内的计算重用同一块数据，大幅减少cache miss，同时浮点运算顺序不变、数值完全一致。] |
| 6 | try_pragma | 0.977x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2); #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)] |
| 7 | rewrite_source | 1.168x  [rewrite: Loop interchange for the second j-loop (j>=i): move the inner k-loop outside the j-loop. For each i, initialize A[i][j] (j from i to N-1) with its original matrix value (already in place), then loop over k=0..i-1, inside which loop j=i..N-1 updating A[i][j] -= A[i][k] * A[k][j]. This makes A[k][j] accesses contiguous (row-major, j varies fastest), while A[i][j] writes are also co |
| 8 | try_flags | 1.163x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>1.6737x</b>（基线 35.85 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb015`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.7018x　最终确认：**1.6737x**　IQR [1.6544, 1.6834]　base_cv=0.6% best_cv=0.5%
- 最终采纳编译选项：`-mllvm -unroll-threshold=1000 -mllvm -licm-max-num-uses-traversed=16`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.007x  [flags: -mllvm -unroll-threshold=300] |
| 2 | rewrite_source | 1.604x  [rewrite: Interchange the loops of the second kernel (the one computing x2) so that the j loop becomes outer and the i loop inner. This changes A[j][i] access to sequential row-major order, drastically improving cache line reuse and enabling auto-vectorization without altering numerical results.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | rewrite_source | 1.596x  [rewrite: Loop tiling for the second kernel: block the i dimension into tiles of size B=256 (or tuned later). For each tile of x2[i0..i0+B-1], iterate over all j and update the tile elements. This keeps a chunk of x2 in L1 cache across the j loop, reducing write-back and read traffic. No change to floating-point accumulation order, thus numerical results remain identical. Also apply same t |
| 5 | try_flags | 1.702x  [flags: -mllvm -unroll-threshold=1000 -mllvm -licm-max-num-uses-traversed=16] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | rewrite_source | 0.993x  [rewrite: 在两个内层循环中引入局部标量累加器：第一个循环，使用局部变量 acc 初始化为 x1[i]，内层循环累加 acc += A[i][j] * y_1[j]，内层结束后写回 x1[i]；第二个循环同理，使用局部变量 acc 初始化为 x2[i]，在内层循环中累加 acc += A[j][i] * y_2[j]（循环交换后 j 为外层，i 为内层，此时 A 的访问已行主序），完成后写回 x2[i]。不改变浮点累加顺序。] |
| 8 | try_flags | 1.000x  [flags: -mllvm -unroll-threshold=1000 -mllvm -licm-max-num-uses-traversed=16] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>1.0124x</b>（基线 1.68 ms，hash 校验，29/51 次为正）</summary>

- 任务 `c2_cb011`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.4107x　最终确认：**1.0124x**　IQR [0.9713, 1.0563]　base_cv=24.7% best_cv=26.0%
- 最终采纳编译选项：`-mllvm -slp-threshold=0 -mllvm -licm-max-num-uses-traversed=32`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.266x  [flags: -mllvm -unroll-threshold=150] |
| 2 | rewrite_source | 1.006x  [rewrite: 对 dijkstra 函数中更新邻居距离的主循环实施缓存分块：将节点划分为固定大小的块（如 256），外层循环遍历块，内层循环处理块内所有节点的距离更新，使用临时数组保存块内距离副本以减少内存冲突，提高 L1 cache 命中率。同时保持原有语义。] |
| 3 | try_flags | 1.411x  [flags: -mllvm -slp-threshold=0 -mllvm -licm-max-num-uses-traversed=32] |
| 4 | try_pragma | 0.842x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 0.824x  [rewrite: 对dijkstra函数中选择未标记节点中距离最小值的扫描循环实施缓存分块：将节点索引范围按块大小256分割，外层循环遍历块，内层循环扫描块内未标记节点并记录局部最小距离及节点编号，内层结束后与全局最小比较更新。同时给距离数组指针添加restrict限定符，消除可能的别名障碍，允许编译器更积极地优化距离更新循环。] |
| 6 | try_flags | 1.374x  [flags: -mllvm -slp-min-tree-size=1] |
| 7 | rewrite_source | 1.119x  [rewrite: 对dijkstra函数中更新邻居距离的主循环实施循环分裂：引入一个栈上固定大小（如1024）的临时数组存储新距离和邻居索引，第一阶段遍历邻接表，计算新距离并存入临时数组，不写入dist；第二阶段依次读取临时数组更新dist。这解除了对dist的读-写交织依赖，降低了存储转发停顿，并允许编译器更好地重排和向量化计算阶段。同时给dist指针添加__restrict限定以消除别名。] |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | try_flags | 1.000x  [flags: -mllvm -slp-threshold=0 -mllvm -licm-max-num-uses-traversed=32] |

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>1.0062x</b>（基线 0.91 ms，hash 校验，29/51 次为正）</summary>

- 任务 `c2_cb012`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0673x　最终确认：**1.0062x**　IQR [0.9836, 1.0205]　base_cv=6.3% best_cv=5.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.067x  [rewrite: Replace the repeated malloc+memset inside the while(fgets) loop with a simple memory pool: allocate a large block once, then carve node and mask structures from it. This reduces the number of calls to malloc, lowers fragmentation, and keeps related data closer in memory for better cache behaviour during trie traversal.] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 1.018x  [rewrite: In the while(fgets) loop, conditionally parse the 'time' field: if(print==1) use sscanf(line, "%f %d", &time, &addr); else use sscanf(line, "%*f %d", &addr) to skip floating‑point conversion when not needed.] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 0.723x  [rewrite: When print==0, call pat_insert directly without first calling pat_search. Only when print==1 do we need the prior search to decide whether to print 'Found'. The semantics are preserved because the original code already calls pat_insert irrespective of key existence when print==0.] |
| 9 | try_flags | 1.021x  [flags: -mllvm -slp-threshold=-4] |

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>1.0147x</b>（基线 3697.92 ms，hash 校验，2/3 次为正）</summary>

- 任务 `c2_pb024`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0040x　最终确认：**1.0147x**　IQR [0.9382, 1.0991]　base_cv=5.5% best_cv=9.1%
- 最终采纳编译选项：`-mllvm -runtime-memory-check-threshold=8`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.004x  [flags: -mllvm -runtime-memory-check-threshold=8] |
| 2 | rewrite_source | 0.975x  [rewrite: Apply cache tiling (loop blocking) to the i and j loops with a block size of 128. The tiled version will iterate over blocks of i and j, performing the original dynamic programming computation on a submatrix that fits in L2/L1 cache, thereby improving locality of table[i][k] and especially table[k+1][j] accesses in the innermost k loop.] |
| 3 | try_pragma | 0.946x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.000x  [flags: -mllvm -runtime-memory-check-threshold=8] |
| 5 | rewrite_source | 0.360x  [rewrite: Exchange the j and k loops: for each i from N-1 down to 0, for each k from i+1 to N-1, for each j from max(k+1, i+1) to N-1, compute table[i][j] using the original updates (including max with table[i][j-1], table[i+1][j], table[i+1][j-1]+match, and the term table[i][k] + table[k+1][j] which now becomes a single addition rather than a max over k). Because the k-loop is moved outsi |

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>0.9943x</b>（基线 1.41 ms，hash 校验，22/51 次为正）</summary>

- 任务 `c2_cb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1058x　最终确认：**0.9943x**　IQR [0.9676, 1.0430]　base_cv=34.7% best_cv=37.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.106x  [rewrite: Precompute the KMP failure function for each find_string once before the main loop, then use a KMP search function (implemented inline) instead of the external strsearch2 call. This turns per-search complexity from O(/haystack/*/needle/) to O(/haystack/+/needle/), and eliminates function call overhead.] |
| 3 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 0.875x  [rewrite: Replace the inline KMP search with Boyer-Moore-Horspool algorithm: precompute a 256-entry bad-character shift table for each needle, and use the BMH scanning loop instead of the KMP state machine. This reduces character comparisons on typical English text.] |
| 6 | try_pragma | 1.000x  [无改善] |
| 7 | rewrite_source | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | try_flags | 0.581x  [rewrite: Convert the inner KMP search while-loop (while (hay[i_t] != '\0')) into a for-loop over explicit haystack length (precomputed via strlen). Declare hay and ndl pointers as __restrict to inform alias analysis. Cache the fail table pointer in a local variable before the loop. This reduces repeated null‑terminator checks, lowers branch overhead, and enables better compiler scheduling |
| 9 |  | 1.000x  [无改善] |

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>0.9888x</b>（基线 1.60 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c2_cb021`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2987x　最终确认：**0.9888x**　IQR [0.8393, 1.1273]　base_cv=19.9% best_cv=21.6%
- 最终采纳编译选项：`-mllvm -unroll-threshold=600`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.153x  [flags: -mllvm -unroll-threshold=600] |
| 2 | rewrite_source | 0.895x  [rewrite(utils/BF_encrypt): 将加密和解密的两个分支合并为一个统一循环：使用局部变量控制 P 数组的遍历方向（正向或反向），然后将所有 BF_ENC 调用放入一个 for (i=1; i<=BF_ROUNDS; i++) 循环中，循环内根据方向索引 p 并调用 BF_ENC，最后处理首尾异或。同时在循环前加入 #pragma clang loop unroll(full) 以确保编译器完全展开，保持原有性能并可能优化指令调度。] |
| 3 | try_pragma | 1.018x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | rewrite_source | 1.299x  [rewrite(utils/BF_encrypt): Change the BF_encrypt function signature from old-style K&R to ANSI C and add 'restrict' qualifiers to the 'data' and 'key' pointer parameters (e.g., BF_LONG * restrict data, const BF_KEY * restrict key). This tells the compiler that data[0..1] do not alias with key->P or key->S, enabling reordering of S-box loads around stores to data and reducing unnecessary s |
| 5 | try_flags | 1.107x  [flags: -mllvm -unroll-threshold=200] |
| 6 | rewrite_source | 1.015x  [rewrite(utils/BF_encrypt): Split the encrypt/decrypt branches in BF_encrypt into two static inline functions (e.g., BF_encrypt_enc and BF_encrypt_dec) that take l,r and key P,S arrays and return updated l,r via pointers. The original BF_encrypt wrapper then calls the appropriate one. This removes the runtime encrypt argument from the hot path and eliminates branch overhead, allowing bette |
| 7 | try_flags | 1.000x  [flags: -mllvm -unroll-threshold=600] |
| 8 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=b4bcb10c12f4, opt=d5192c8b17e4) |
| 9 | try_pragma | 0.927x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>0.9977x</b>（基线 1.37 ms，hash 校验，22/51 次为正）</summary>

- 任务 `c2_cb020`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.4101x　最终确认：**0.9977x**　IQR [0.9917, 1.0063]　base_cv=6.0% best_cv=6.0%
- 最终采纳编译选项：`-mllvm -tail-dup-size=6`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpor0tnce8/rw_utils_utils_shadow/polybench.c:1091:1: error: expected expression |
| 3 | try_pragma | 0.972x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.410x  [flags: -mllvm -tail-dup-size=6] |
| 5 | rewrite_source | 1.081x  [rewrite(utils/BF_encrypt): Switch to ANSI C prototype with restrict on data and key. Copy key->P into a local array p_local[BF_ROUNDS+2] and use p_local instead of p, keeping S as is. Do not expand BF_ENC or modify the macro.] |
| 6 | try_flags | 1.000x  [flags: -mllvm -tail-dup-size=6] |
| 7 | try_pragma | 1.026x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=002de6ea4c73, opt=f61642852771) |
| 9 | try_flags | 1.000x  [flags: -mllvm -tail-dup-size=6] |

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>1.0025x</b>（基线 1.18 ms，hash 校验，33/51 次为正）</summary>

- 任务 `c2_cb014`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0700x　最终确认：**1.0025x**　IQR [0.9968, 1.0170]　base_cv=10.7% best_cv=10.0%
- 最终采纳编译选项：`-mllvm -unroll-runtime=false`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.974x  [rewrite: Replace the hexadecimal-to-byte conversion while-loop with a lookup table (256 bytes) that maps ASCII chars directly to nibble values, eliminating the chained if-else branches. Then accumulate bytes using the table for each pair. This targets the key-parsing loop (while(i < 64 && *cp)). No arrays are resident in this loop, but the key[] array is written; the transformation improv |
| 3 | try_pragma | 0.937x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.029x  [flags: -mllvm -unroll-threshold=100] |
| 5 | rewrite_source | 失败 [rewrite_source] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpok4_0w13/security_rijndael_decode_rewrite.c:167:1: error: unknown type name 'unchanged' |
| 6 | try_flags | 1.070x  [flags: -mllvm -unroll-runtime=false] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 失败 [rewrite_source] output hash mismatch (ref=2a9d644f22dd, opt=86d5d9352c6d) |
| 9 | try_flags | 1.030x  [flags: -mllvm -slp-threshold=-1] |

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>1.0004x</b>（基线 1.14 ms，hash 校验，26/51 次为正）</summary>

- 任务 `c2_cb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.6265x　最终确认：**1.0004x**　IQR [0.9554, 1.0350]　base_cv=6.4% best_cv=7.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.861x  [rewrite: Convert the hex key parsing loop into a table‑driven decoder that processes two hexadecimal characters per iteration using a 256‑byte mapping array, eliminates the i&1 conditional store, and hoists the length check to reduce loop‑carried branches.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 1.552x  [rewrite: Rewrite the hex key parsing loop to consume two hex characters per iteration using direct bitwise computation (no lookup table). Each iteration reads two chars, computes a byte, and stores it to key[out_idx++]. Hoist the input length validation before the loop, and add a final single-char handler for odd-length keys. This eliminates the conditional store on odd i and the division |
| 6 | rewrite_source | 1.626x  [rewrite: Precompute the input string length with __builtin_strlen, reject it if not exactly 32, 48, or 64 hex digits, then use a for (int idx = 0; idx < len; idx += 2) loop to decode two hex chars at a time with no null checks. The trailing single-char handling and the post-loop length checks become dead code and can be removed. This eliminates all per-iteration null tests and simplifies  |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_pragma | 1.004x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>1.0000x</b>（基线 1.17 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c2_cb016`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.732x  [rewrite(utils/sha_stream): 将 while 循环中的逐次 fread(data,1,BLOCK_SIZE,fin) 改为一次读取较大块（如 128 KB 的局部缓冲区 buf），然后内部循环以 BLOCK_SIZE 为步长传递 buf 片段给 sha_update，最后处理剩余不足 BLOCK_SIZE 的尾部。这样可以大幅减少 fread 调用次数，降低库调用开销，同时保持 sha_update 的调用序列与原始完全相同，确保数值一致。]content empty, falling back to reasoning_content on attempt 1 |
| 3 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpq0rawizp/rw_utils_utils_shadow/polybench.c:767:5: error: use of undeclared identifier 'uint32_t' |
| 6 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp7pnejbda/rw_utils_utils_shadow/polybench.c:771:37: error: too many arguments to function call, expected single argument 'sha_info', have 2 arguments |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>1.0167x</b>（基线 18858.79 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c2_pb030`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0167x　最终确认：**1.0167x**　IQR [1.0166, 1.0167]　base_cv=0.0% best_cv=0.0%
- 最终采纳编译选项：`-mllvm -unroll-allow-partial=1`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.002x  [flags: -mllvm -slp-threshold=-4 -mllvm -slp-min-tree-size=2] |
| 2 | rewrite_source | 1.000x  [rewrite: 对 i 循环进行 cache tiling（分块），以固定大小的行块（例如 32 行）为单位处理每个时间步内的 i 循环，减少对 A 数组的 cache miss，提升访存局部性。] |
| 3 | try_flags | 1.017x  [flags: -mllvm -unroll-allow-partial=1] |
| 4 | try_pragma | 0.995x  [pragma: #pragma clang loop unroll_count(4); #pragma clang loop unroll_count(8)] |
| 5 | rewrite_source | 1.012x  [rewrite: 对内层 j 循环进行标量展开（unroll‑and‑jam），展开因子取 4。使用局部标量变量显式缓存 A[i‑1][j‑1..j+4]、A[i][j‑1..j+3] 以及 A[i+1][j‑1..j+4] 中的重用部分，每次计算连续 4 个 A[i][j] 的值，保证更新顺序与原始串行完全一致，从而利用寄存器复用减少对 A 数组的重复 load，降低内存访问开销。] |
| 6 | try_flags | 1.001x  [flags: -mllvm -slp-threshold=-4] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 1.011x  [rewrite: 对内层 j 循环实施展开因子为2的手动 scalar unroll‑and‑jam：每轮迭代同时计算两个连续的 A[i][j] 和 A[i][j+1]，利用显式标量变量缓存相邻 stencil 元素的重叠部分，严格保持原始串行更新顺序，以降低内存 load 次数并减轻寄存器溢出。] |
| 9 | try_flags | 1.001x  [flags: -mllvm -slp-threshold=-5] |

</details>

<details><summary><b>symm</b> — 中位加速比 <b>4.3165x</b>（基线 939.57 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb006`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：4.5795x　最终确认：**4.3165x**　IQR [4.2246, 4.3806]　base_cv=1.0% best_cv=2.0%
- 最终采纳编译选项：`-mllvm -unroll-threshold=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.008x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 2 | rewrite_source | 4.433x  [rewrite: Loop interchange: move the j loop innermost (i -> k -> j order). Introduce a temporary scalar array temp2_acc[N] initialized to 0 for each i, updated in the k loop as temp2_acc[j] += B[k][j]*A[i][k]. Then, after the k loop, use a final j loop to update C[i][j] = beta*C[i][j] + alpha*B[i][j]*A[i][i] + alpha*temp2_acc[j]. This preserves the original FP accumulation order exactly.] |
| 3 | try_flags | 4.524x  [flags: -mllvm -slp-threshold=-2] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 3.569x  [rewrite: Apply cache tiling (blocking) on the j dimension with tile size 64. For each i, first zero temp2_acc for the entire tile, then iterate k and the j-tile inner loop to update C[k][j] and temp2_acc[j], and finally update C[i][j] for the tile. This preserves the original accumulation order (k outer, j inner and j monotonic within a tile) and improves spatial/temporal locality of temp |
| 6 | try_flags | 4.572x  [flags: -mllvm -slp-min-tree-size=4] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 0.973x  [rewrite: 在 kernel_symm 的函数参数声明中，为 C、A、B 三个二维数组参数添加 restrict 修饰符，告知编译器这些指针不与其他变量别名（包括局部数组 temp2_acc），从而允许 cost model 更激进地执行向量化、循环交换和指令调度，同时保持最优 flags -mllvm -slp-min-tree-size=4。] |
| 9 | try_flags | 4.580x  [flags: -mllvm -unroll-threshold=200] |

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>2.9772x</b>（基线 1168.11 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb007`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：3.1399x　最终确认：**2.9772x**　IQR [2.8929, 2.9951]　base_cv=1.5% best_cv=0.5%
- 最终采纳编译选项：`-mllvm -prefetch-distance=512`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.010x  [flags: -mllvm -prefetch-distance=64] |
| 2 | rewrite_source | 2.974x  [rewrite: 将 j 循环与 k 循环交换，使 j 变为外层、k 变为最内层循环，从而 A[j][k]、B[j][k]、A[i][k]、B[i][k] 全部沿 k 方向连续访问；同时可考虑引入标量累加器，将 C[i][j] 的更新推迟到内层 k 循环结束后一次性写入。] |
| 3 | try_flags | 2.952x  [flags: -mllvm -slp-threshold=20] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 3.103x  [rewrite: 对 i 和 j 循环进行 cache tiling，分块大小 TI=64, TJ=64。外层 i 以 TI 步长分块，内层 j 以 TJ 步长分块（保持 j≤i 约束），k 仍为最内层不变。在分块内对每个 (i,j) 仍使用标量累加器 tmp，遍历全部 k 后最终写回 C[i][j]。不拆分 k 循环，以保留连续访问优势，同时通过 i、j 分块使 A 和 B 中当前被用到的行集驻留在缓存中。] |
| 6 | try_flags | 3.140x  [flags: -mllvm -prefetch-distance=512] |
| 7 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 5.15e-03 at index 1920 (ref=1.94, opt=1.93), epsilon=1.00e-04 |
| 8 | rewrite_source | 3.127x  [rewrite: 在 kernel_syr2k 函数的参数列表中为 DATA_TYPE POLYBENCH_2D 数组 C, A, B 添加 __restrict__ 限定符（示例：DATA_TYPE (* __restrict__ C)[N]），以显式告知编译器这些指针不会别名，促使向量化和指令级并行；同时将 TI、TJ 分块大小从 64 调整为 128 以更好匹配 25 MiB L2 缓存，提升 A[j][:] 和 B[j][:] 在 i 循环重的重用率。保持现有循环结构和数值顺序不变。] |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>1.7237x</b>（基线 1021.38 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb008`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.7727x　最终确认：**1.7237x**　IQR [1.6915, 1.7276]　base_cv=0.7% best_cv=0.5%
- 最终采纳编译选项：`-mllvm -slp-max-reg-size=256`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 2 | try_flags | 1.315x  [rewrite: 交换 j 和 k 循环，将内层变为 k 循环，使 A[i][k] 和 A[j][k] 均为连续访问；同时融合 beta 乘法到同一 j 循环中，即在 j 循环体内先执行 C[i][j] *= beta，再进行 k 累加，避免对 C 的两次遍历。] |
| 3 | rewrite_source | 1.000x  [无改善] |
| 4 | try_pragma | 1.488x  [rewrite: 对 i 循环进行 tile 分块，块大小 64；在块内保持 i、j≤i、k 的顺序，对 k 循环手工展开因子 2，将 k 和 k+1 两个连续迭代合并为一个循环体，先累加 A[i][k]*A[j][k] 再累加 A[i][k+1]*A[j][k+1]，循环步长为 2；剩余迭代用原顺序补齐。保持单个 sum 累加器以维持原始浮点运算顺序。] |
| 5 | try_flags | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 9.46e-04 at index 3202 (ref=10.57, opt=10.56), epsilon=1.00e-04 |
| 6 | try_pragma | 1.000x  [无改善] |
| 7 | rewrite_source | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 9.46e-04 at index 3202 (ref=10.57, opt=10.56), epsilon=1.00e-04 |
| 8 | try_flags | 1.572x  [rewrite: 对j循环也进行分块，块大小64，在i分块内部再对j分块，形成二维分块以进一步减少C矩阵的缓存缺失，保持k循环内累加顺序不变。] |
| 9 |  | 1.773x  [flags: -mllvm -slp-max-reg-size=256] |

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>1.0055x</b>（基线 1.91 ms，hash 校验，26/51 次为正）</summary>

- 任务 `c2_cb017`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1190x　最终确认：**1.0055x**　IQR [0.9611, 1.0619]　base_cv=19.3% best_cv=19.2%
- 最终采纳编译选项：`-mllvm -machine-combiner-inc-threshold=4`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.119x  [flags: -mllvm -machine-combiner-inc-threshold=4] |
| 2 | rewrite_source | 0.618x  [rewrite(utils/adpcm_coder): 对 adpcm_coder 的主迭代循环 (for ( ; len > 0 ; len--)) 进行 4 倍展开（外层 for len >= 4 处理 4 个样本，剩余尾部单独处理），并在展开体内将 diff 与 step 的 if 比较替换为 -(diff >= step) 掩码的条件加减，同时把 sign 比较和 valpred 钳位也改写为无分支形式，以消除分支预测失误并暴露更多的指令级并行，bufferstep 展开时按位正确拼接输出字节。] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [flags: -mllvm -machine-combiner-inc-threshold=4] |
| 5 | rewrite_source | 1.021x  [rewrite(utils/adpcm_coder): 对adpcm_coder主循环 `for ( ; len > 0 ; len-- )` 进行2倍展开，每次处理两个样本，并在函数体内将局部指针 `inp` 和 `outp` 声明为 `__restrict` 以消除别名；保持原有diff与step比较的分支代码不变，调整bufferstep逻辑使两个样本的输出字节正确拼接；使用base=current_best继承已有的 -machine-combiner-inc-threshold=4 标志。] |
| 6 | rewrite_source | 1.024x  [rewrite(utils/adpcm_coder): 在 adpcm_coder 函数内，将局部指针 inp、outp 声明为 short *__restrict 和 signed char *__restrict，并在主循环体开头添加 __builtin_prefetch(&stepsizeTable[index+1], 0, 3) 及 __builtin_prefetch(&indexTable[index+1], 0, 1)（注意边界检查），以消除别名导致的保守优化并提前缓存下一次查表所需数据；保持原有循环结构、分支逻辑及所有依赖关系不变。] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | try_pragma | 0.904x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.000x  [flags: -mllvm -machine-combiner-inc-threshold=4] |

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>1.0032x</b>（基线 1.48 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c2_cb018`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.2895x　最终确认：**1.0032x**　IQR [0.9857, 1.0245]　base_cv=29.0% best_cv=29.8%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.995x  [rewrite(utils/adpcm_decoder): Unroll the main loop by a factor of 2 to process two output samples per iteration. Each iteration reads one input byte, extracts the high and low nibbles in order, applies ADPCM decoding to each, and writes two output samples. This removes the bufferstep toggle, reduces loop overhead, and may improve ILP.] |
| 3 | try_pragma | 1.014x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.167x  [flags: -mllvm -unroll-threshold=500 -mllvm -licm-max-num-uses-traversed=8 -mllvm -slp-threshold=0] |
| 5 | rewrite_source | 1.289x  [rewrite(utils/adpcm_decoder): Unroll the main loop by a factor of 4, processing 2 input bytes (4 nibbles) per iteration. Eliminate the bufferstep toggle by reading a full byte and using two internal stages per nibble within the iteration. Store frequently used lookup tables (stepsizeTable, indexTable) in local variables to reduce address calculations. Keep the sequential state updates for |
| 6 | try_flags | 1.222x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -tail-dup-size=2000] |
| 7 | rewrite_source | 0.987x  [rewrite(utils/adpcm_decoder): Further unroll the main loop from 4 to 8 output samples per iteration (processing 4 input bytes). Declare input/output pointers with `restrict` to resolve potential aliasing concerns, and load the global lookup tables `stepsizeTable` and `indexTable` into local `const int*` pointers to reduce address computations. Keep all sequential state updates intact to p |
| 8 | try_pragma | 0.927x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | rewrite_source | 0.984x  [rewrite(utils/adpcm_decoder): Add `restrict` qualifiers to `indata` and `outdata` pointer parameters, and load the content of global arrays `stepsizeTable` and `indexTable` into local `static const int` arrays at function entry. Use these local copies inside the 4-way unrolled loop. Keep the loop structure and state updates identical to the current best version.] |

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>1.0063x</b>（基线 2.07 ms，hash 校验，28/51 次为正）</summary>

- 任务 `c2_cb019`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：2.3122x　最终确认：**1.0063x**　IQR [0.9768, 1.0540]　base_cv=8.7% best_cv=10.2%
- 最终采纳编译选项：`-mllvm -inline-threshold=300`
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.930x  [flags: -mllvm -max-speculation-depth=5] |
| 2 | rewrite_source | 0.940x  [rewrite: 将 crc32file 中现有的逐字节 CRC 更新循环改写为按 4 字节字处理的 slice-by-4 形式：每次迭代读取 4 字节，使用预计算的 4 字节 CRC 查找表一次性更新 CRC 累加器，并将内层手动展开为 4 次查表操作，以减少循环迭代次数和 L1 缓存压力，同时保持与原算法严格一致的 CRC-32 结果。] |
| 3 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 4 | try_flags | 1.486x  [flags: -mllvm -enable-misched=0] |
| 5 | rewrite_source | 1.052x  [rewrite: Rewrite the inner loop of crc32file to process the input buffer in 32-byte or 64-byte blocks using four independent CRC accumulators (via _mm_crc32_u64 or __builtin_ia32_crc32di). Unroll the loop 4×, computing CRC on four disjunct 8-byte windows in parallel, then combine the four partial CRCs using the crc32 folding formula at the end. Add #pragma clang loop unroll(4) to assist t |
| 6 | try_flags | 2.312x  [flags: -mllvm -inline-threshold=300] |
| 7 | try_pragma | 0.872x  [pragma: #pragma clang loop unroll(enable)] |
| 8 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpf_iryqfz/telecom_crc32_rewrite.c:129:1: error: unknown type name '我们被要求优化' |
| 9 | try_flags | 1.000x  [flags: -mllvm -inline-threshold=300] |

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>1.0885x</b>（基线 10.85 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb021`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.1188x　最终确认：**1.0885x**　IQR [1.0777, 1.0927]　base_cv=2.0% best_cv=2.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.038x  [rewrite: 手动将内层循环展开 4 轮，使用 scalar accumulator 临时变量累加 L[i][j]*x[j]，然后一次性从 x[i] 中减去。展开循环的顺序与原始循环一致（逐元素累加不重排），以保持数值 bit-exact，同时减少循环分支并鼓励编译器生成更宽的 SIMD 指令。] |
| 3 | try_flags | 1.019x  [flags: -mllvm -slp-min-tree-size=5] |
| 4 | try_pragma | 1.119x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 0.942x  [rewrite: 实现 blocked forward substitution：将 n 均分为块大小 B（如 64），外层循环按块递增 k，每块内先用现有展开及 vectorize pragma 的内层循环求解块内行的 x（对角块处理），然后遍历后续行用块内已知 x 更新其 b 值，以提升 L 矩阵列块的缓存重用和 x 向量的访问局部性。] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_pragma | 1.111x  [pragma: #pragma clang loop interleave(enable) interleave_count(2)] |
| 8 | rewrite_source | 0.920x  [rewrite: 从 current_best 出发，将内层手动展开循环宽度从 4 扩展到 8：for (j = 0; j + 7 < i; j += 8)，使用 8 路标量累加器依次减去 L[i][j] 到 L[i][j+7] 与 x[j] 到 x[j+7] 的乘积，尾循环保持不变。在同一循环前添加 #pragma clang loop vectorize(enable) vectorize_width(8)（若硬件支持 512 位 SIMD）或 vectorize_width(4)，以强制编译器使用更宽向量。] |
| 9 | try_flags | 1.007x  [flags: -mllvm -slp-threshold=-1] |

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>7.3446x</b>（基线 506.42 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c2_pb009`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：7.4840x　最终确认：**7.3446x**　IQR [6.8825, 7.4143]　base_cv=1.3% best_cv=2.9%
- 最终采纳编译选项：`-mllvm -unroll-threshold=300`
- 最终采纳了源码重写
- 实际获得的反馈通道：`none`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 7.438x  [rewrite: 循环交换：将 j 循环移到最内层，形成 i (外层) → k (中层) → j (内层) 的顺序，使内层循环对 B[i][j] 和 B[k][j] 的访问连续；同时保持 alpha 乘法在 k 循环之后正确执行。] |
| 3 | try_flags | 7.484x  [flags: -mllvm -unroll-threshold=300] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 5.619x  [rewrite: Loop tiling on the j dimension: partition the innermost j loop into blocks of size TJ=64, inserting a jj loop inside the k loop but outside the existing j loop. The new order becomes i → k → jj (stride TJ) → j (jj to min(jj+TJ, N)). This keeps B[i][0..N-1] and B[k][0..N-1] in L1 across the k iterations for each tile, reducing cache misses without changing numerical results.] |
| 6 | try_flags | 6.288x  [flags: -mllvm -slp-threshold=2] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 6.167x  [rewrite: 对 j 维度进行 cache blocking：将原 j 循环分成大小为 BJ=64 的块，插入外层 jj 循环 (步长 BJ)，内层 j 从 jj 到 min(jj+BJ, N)。然后将 k 循环移入 jj 循环内部、j 循环外部，形成顺序 i → jj → k → j。在 k 循环内部执行 B[i][j] += A[k][i] * B[k][j] 累加。alpha 乘法仍放在所有 k 累加完成后（i 循环末尾）对整个 j 进行，数值完全一致。此变换使 B[i] 的每一 tile 驻留 L1 跨 k 复用，避免缓存颠簸，而内层 j 仍保持连续以支持向量化，预计显著降低内存流量并提升性能。] |
| 9 | try_flags | 6.864x  [flags: -mllvm -unroll-threshold=300] |

</details>

### ③ full system（自由选动作 + 完整编译器反馈）（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>9.1823x</b>（基线 3758.70 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb010`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：9.9517x　最终确认：**9.1823x**　IQR [5.0011, 10.5264]　base_cv=7.7% best_cv=42.8%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=16`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 4.947x  [rewrite: 将两个累加循环的中间维度与内层维度交换：第一个乘法的 for(j) 和 for(k) 交换，变成 for(i) for(k) for(j)，使 B[k][j] 访问连续并提取标量因子 alpha*A[i][k]；第二个乘法的 for(j) 和 for(k) 交换，变成 for(i) for(k) for(j)，使 C[k][j] 访问连续。] |
| 3 | try_flags | 5.058x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 6.479x  [rewrite: 对第一个乘法的 i 和 k 循环进行二重 cache tiling（blocking），块大小 BI=32, BK=32；同样对第二个乘法的 i 和 k 循环进行相同分块。保持 j 循环完全连续，即对于每个分块后的 i 块和 k 块，完整执行内层 j 循环。这样 B/C 矩阵的块在分块内被重复使用，减少缓存缺失。] |
| 6 | try_flags | 5.613x  [flags: -mllvm -vectorize-scev-check-threshold=8] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 9.952x  [rewrite: 在函数声明的所有数组参数（tmp, A, B, C, D）前添加 restrict 限定符，告知编译器这些指针不重叠，从而让 LICM 成功外提剩余的两个 loop-invariant load，并降低 LoopVectorizer 的内存检查开销，促使内层 j 循环向量化。] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>6.8451x</b>（基线 3709.88 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb011`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：6.3070x　最终确认：**6.8451x**　IQR [5.6791, 7.1824]　base_cv=12.5% best_cv=0.7%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 5.968x  [rewrite: 将三个矩阵乘法段落的循环顺序由原来的i(外层)-j(中层)-k(内层)交换为i(外层)-k(中层)-j(内层)，使对B、D、F的访问变为行主序连续，同时保持每个输出元素的累加顺序不变（按k递增）。] |
| 3 | try_flags | 5.831x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 4 | try_pragma | 6.033x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 6.051x  [rewrite: Apply loop tiling (cache blocking) to all three matrix multiplication loops. For each product, tile the outer i loop and the inner k loop with a block size of 64, keeping the original i-k-j loop order and leaving the innermost j loop intact. This ensures that sub-blocks of A, B and E (respectively C, D, F and E, F, G) fit in L1 cache, increasing data reuse across j iterations and |
| 6 | rewrite_source | 5.824x  [rewrite: 对三个矩阵乘法的每个 tile，在 k 块循环内引入局部 double 数组 acc[NJ]（每个 j 维度的累加器），将循环体 E[i][j] += A[i][k] * B[k][j]（及 F、G 对应）替换为 acc[j] += A[i][k] * B[k][j]，并在 k 块结束后一次性将 acc[j] 累加到相应输出行。这消除了内层循环对 E/F/G 的读-改-写操作，减少内存流量，消除 aliasing 依赖，有望触发 LoopVectorizer 的 SIMD 向量化。累加顺序保持 k 递增且 j 遍历顺序不变，数值结果与原算法一致。] |
| 7 | try_pragma | 失败 [try_pragma] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp7mfmynm9/3mm_pragma.c:101:21: error: duplicate directives 'vectorize(enable)' and 'vectorize(enable)' |
| 8 | rewrite_source | 6.307x  [rewrite: For all three matrix multiplication blocks (E=A*B, F=C*D, G=E*F), inside the i and k tile loops, before the innermost j loop, load the k-loop-invariant scalar element (A[i][k], C[i][k], E[i][k] respectively) into a local double variable. Replace the use inside the j loop with that local variable. This manually hoists the missed LICM loads, reduces scalar load traffic, and lowers  |
| 9 | try_flags | 5.826x  [flags: -mllvm -licm-max-num-uses-traversed=32] |

</details>

<details><summary><b>adi</b> — 中位加速比 <b>1.0310x</b>（基线 13927.82 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb025`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.5837x　最终确认：**1.0310x**　IQR [1.0303, 1.0318]　base_cv=0.0% best_cv=0.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.979x  [rewrite: Apply cache tiling (i-loop blocking) to both the Column Sweep and Row Sweep: wrap the existing for (i=1; i<_PB_N-1; i++) loop with an outer ii loop (tile size e.g., 64) such that each block processes a contiguous range of i indices before moving to the next block. Inside each block, retain the original forward and backward j sweeps unchanged. This transformation improves spatial  |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀content empty, falling back to reasoning_content on attempt 1 |
| 4 | rewrite_source | 0.968x  [rewrite: Loop interchange on both the Column Sweep and Row Sweep: replace the original nest (for i=1..N-2 outer, for j=1..N-2 inner) with a nest where the j dimension becomes the outer loop and the i dimension the inner loop. In the Column Sweep, for j=1..N-2 compute p[i][j] and q[i][j] for all i first, then in a separate reversed j loop (j=N-2..1) compute v[j][i] for all i. Apply the sam |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | rewrite_source | 1.462x  [rewrite: Replace the 2D arrays p and q with local 1D arrays (size N) inside the kernel. In each i-loop, allocate double p_row[N], q_row[N] and use them instead of p[i][j], q[i][j]; do not store back to the passed p/q arrays. This eliminates O(N²) temporary stores and loads, reducing memory footprint and improving cache reuse for the actual data arrays u and v.]content empty, falling back  |
| 8 | try_flags | 1.584x  [flags: -mllvm -partial-unrolling-threshold=500 -mllvm -slp-max-vf=8] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>atax</b> — 中位加速比 <b>1.0661x</b>（基线 21.59 ms，numeric 校验，25/25 次为正）</summary>

- 任务 `c3_pb012`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1073x　最终确认：**1.0661x**　IQR [1.0391, 1.1152]　base_cv=2.9% best_cv=3.3%
- 最终采纳编译选项：`-mllvm -slp-threshold=-3`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.015x  [rewrite: Add 'restrict' qualifier to all pointer parameters (A, x, y, tmp) in the kernel_atax signature to enable LICM to hoist loop-invariant loads and potentially permit more aggressive SIMD vectorization.] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 1.001x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.038x  [rewrite: 将第一个j循环中的tmp[i]累加改为scalar accumulator：声明局部double s=0.0；循环内s += A[i][j]*x[j]；循环后tmp[i]=s；减少对tmp[i]的重复load/store。] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_pragma | 1.009x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.067x  [rewrite: 手动展开外层i循环，展开因子2：每次迭代处理两个连续的i行，声明两个独立的标量累加器s0和s1，分别计算tmp[i]和tmp[i+1]的点积，然后使用相应的tmp_i_val0和tmp_i_val1依次更新y[]，保证浮点累加顺序与原始完全一致，余数处理最后奇数行。同时通过also_flags启用更激进的SLP阈值(-1)和LICM最大使用数(64)，尝试消除被拒绝的向量化和提升外提。] |
| 9 | try_flags | 1.107x  [flags: -mllvm -slp-threshold=-3] |

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>1.0003x</b>（基线 13.91 ms，hash 校验，22/39 次为正，⚠ 正确性门无效）</summary>

- 任务 `c3_cb001`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0667x　最终确认：**1.0003x**　IQR [0.9988, 1.0020]　base_cv=0.8% best_cv=0.6%
- 最终采纳编译选项：`-mllvm -licm-mssa-optimization-cap=16 -mllvm -licm-max-num-uses-traversed=8`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp_2wthyra/rw_utils_utils_shadow/polybench.c:639:7: error: use of undeclared identifier 'my3DVertexStruct' |
| 3 | try_pragma | 0.991x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp2y211pg0/rw_utils_utils_shadow/polybench.c:641:14: error: use of undeclared identifier 'my3DVertexStruct' |
| 6 | try_flags | 1.008x  [flags: -mllvm -vectorizer-min-trip-count=8] |
| 7 | try_pragma | 1.004x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.062x  [rewrite(utils/qsortx): Modify qsortx signature in utils/polybench.c to accept an additional 'unsigned dist_off' parameter (byte offset of distance field). Inside the partition loop, replace the indirect calls 'comp(loguy, lo)' and 'comp(higuy, lo)' with direct integer comparisons: '*(int*)(loguy + dist_off) <= *(int*)(lo + dist_off)' and '*(int*)(higuy + dist_off) >= *(int*)(lo + dist_off |
| 9 | try_flags | 1.067x  [flags: -mllvm -licm-mssa-optimization-cap=16 -mllvm -licm-max-num-uses-traversed=8] |

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>1.0004x</b>（基线 6.11 ms，hash 校验，27/51 次为正）</summary>

- 任务 `c3_cb002`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2001x　最终确认：**1.0004x**　IQR [0.9918, 1.0122]　base_cv=1.7% best_cv=1.4%
- 最终采纳编译选项：`-mllvm -slp-max-vf=8 -mllvm -vectorize-num-stores-pred=2 -mllvm -slp-max-reg-size=256`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.200x  [flags: -mllvm -slp-max-vf=8 -mllvm -vectorize-num-stores-pred=2 -mllvm -slp-max-reg-size=256] |
| 2 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): output hash mismatch (ref=7fdd17536753, opt=eabff96268d0) |
| 3 | try_pragma | 1.018x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.191x  [flags: -mllvm --unroll-max-upperbound=128] |
| 5 | rewrite_source | 失败 [rewrite_source] precision error (fix also failed): output hash mismatch (ref=7fdd17536753, opt=98654f0daf95) |
| 6 | try_flags | 1.068x  [flags: -mllvm -slp-max-look-ahead-depth=4] |
| 7 | try_pragma | 1.041x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp8lq3s6o0/automotive_susan_corners_rewrite.c:1745:1: error: function definition is not allowed here |
| 9 | try_flags | 1.198x  [flags: -mllvm -slp-max-root-look-ahead-depth=8 -mllvm -licm-max-num-uses-traversed=64] |

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>1.0172x</b>（基线 8.00 ms，hash 校验，50/51 次为正）</summary>

- 任务 `c3_cb003`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0380x　最终确认：**1.0172x**　IQR [1.0158, 1.0187]　base_cv=1.0% best_cv=1.0%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=300 -mllvm -vectorize-num-stores-pred=16 -mllvm -slp-max-vf=4`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 失败 [rewrite_source] optimized run timed out |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.038x  [flags: -mllvm -partial-unrolling-threshold=300 -mllvm -vectorize-num-stores-pred=16 -mllvm -slp-max-vf=4] |
| 5 | rewrite_source | 0.898x  [rewrite: In the susan_thin function at automotive_susan_edges.c around lines 589-597 where n accumulates via several n+=*(cp-*p++) operations, split the single 'n' accumulator into 4 scalar accumulators (e.g., n0,n1,n2,n3) used in round-robin order, and sum them after the loop. This breaks the loop-carried reduction dependence, exposes instruction-level parallelism, and may allow SLP vect |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300 -mllvm -vectorize-num-stores-pred=16 -mllvm -slp-max-vf=4] |
| 8 | rewrite_source | 1.017x  [rewrite: In susan_thin’s innermost loop (around lines 589-597 and any similar conditional accumulation), replace branches like 'if (cond) { acc += val; }' with branch-free forms such as 'int mask = (cond) ? 1 : 0; acc += val * mask;' or using explicit select intrinsics. This eliminates control flow that blocks vectorization while preserving the original scalar reduction semantics and nume |
| 9 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300 -mllvm -vectorize-num-stores-pred=16 -mllvm -slp-max-vf=4] |

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.1664x</b>（基线 61.88 ms，hash 校验，9/9 次为正）</summary>

- 任务 `c3_cb004`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1655x　最终确认：**1.1664x**　IQR [1.1650, 1.1683]　base_cv=0.2% best_cv=0.1%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=400`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.999x  [rewrite: 对 susan_smoothing 中遍历图像像素的嵌套循环进行二维循环分块（tiling），块大小 32×32 或 64×64，配合内层累加器展开为 explicit vectorizable reductions（例如将 n+=*(cp-*p++) 的手动展开改为连续地址访存的累加模式，消除指针间接寻址），并提升循环不变量的预计算（如模板系数），减少内层重复计算。] |
| 3 | try_pragma | 失败 [try_pragma] 优化版编译失败 (SMALL_DATASET): clang-21: error: unknown argument: '-slp-threshold=-1' |
| 4 | try_flags | 1.010x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm --instcombine-negator-max-depth=8] |
| 5 | rewrite_source | 1.150x  [rewrite: 将 susan_smoothing 的中心滤波循环（通过指针列表 cp-*p++ 计算求和 n）改写为基于预计算掩模偏移数组的直接索引访问：声明 int mask_offsets[MASK_SIZE] 存储相对于中心像素的线性偏移量，内循环改为 for (j=0; j<MASK_SIZE; j++) { int idx = center_idx + mask_offsets[j]; sum += in_image[idx]; }；同时将 max_r 的 if-赋值替换为条件选择 max_r = (r[i] > max_r) ? r[i] : max_r；以消除间接指针追逐和分支，使 SLP 能向量化归约。] |
| 6 | try_flags | 1.166x  [flags: -mllvm --partial-unrolling-threshold=400] |
| 7 | try_pragma | 1.001x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.155x  [rewrite: 对 large Gaussian masks 分支的 double‑nested i/j 输出循环进行二维 cache tiling：块大小 64×64，仍保持 i,j 原遍历顺序，为每个 tile 限定输入图像的有效窗口区域；内层 mask 循环的 ip 起点仍基于原始的中心像素计算，不改变每个输出点的累加顺序。通过将 tile 限定在 L1d 内部以提升输入像素的重用。] |
| 9 | try_flags | 1.123x  [flags: -mllvm -partial-unrolling-threshold=800] |

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>1.6654x</b>（基线 32.63 ms，numeric 校验，15/15 次为正）</summary>

- 任务 `c3_pb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.7310x　最终确认：**1.6654x**　IQR [1.5994, 1.6730]　base_cv=2.4% best_cv=3.5%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.590x  [rewrite: In the i loop, load r[i] into a local double r_i_val before the j loop, and replace q[i] accumulation with a local double q_i_acc initialized to 0.0, then assign q_i_acc to q[i] after the j loop. This manual hoisting and scalar accumulation remove aliasing concerns for r[i] and q[i], allowing the vectorizer to succeed on the inner j loop.] |
| 3 | try_flags | 1.457x  [flags: -mllvm -instcombine-max-sink-users=1000] |
| 4 | try_pragma | 1.731x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.447x  [rewrite: 对 j 循环进行分块（blocking），块大小为 BJ（例如 64 或 128）。在 j 块内使用局部数组 s_tile[BJ] 累积 s[j] 的贡献，同时保持 q_i_acc 的累加。j 块结束后将 s_tile 累加到全局 s。这样减少了 s 数组的 load-store 次数，并提高了 p[j] 在 j 块内的缓存复用，从而降低内存带宽压力。] |
| 6 | try_flags | 1.659x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 7 | try_pragma | 失败 [try_pragma] 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpb0wsvw2a/bicg_pragma.c:91:21: error: duplicate directives 'vectorize(enable)' and 'vectorize(enable)' |
| 8 | rewrite_source | 1.496x  [rewrite: 手动展开内层 j 循环 4 次：将 j 循环改为步长 4 的循环，每次迭代计算 s[j]、s[j+1]、s[j+2]、s[j+3] 以及对应的 A[i][j]、p[j] 访问，并利用 4 个标量累加器分段累加 q_i_acc（最后合并）。循环末尾处理余数迭代。此变换不改变加法顺序（仍为原始顺序），但增加了基本块内的独立 FMA 操作数，有助于 ILP 和向量化。] |
| 9 | try_flags | 1.610x  [flags: -mllvm -pragma-vectorize-scev-check-threshold=16] |

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>1.0393x</b>（基线 114.63 ms，hash 校验，5/9 次为正）</summary>

- 任务 `c3_cb005`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.5793x　最终确认：**1.0393x**　IQR [0.9038, 1.0959]　base_cv=17.9% best_cv=35.5%
- 最终采纳编译选项：`-mllvm -unroll-max-percent-threshold-boost=200`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.304x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 2 | rewrite_source | 1.014x  [rewrite: 对 kernel_bzip2_decode 中频繁读写的全局状态变量（smallMode、blockSize100k、verbosity、forceOverwrite、keepInputFiles、noisy、opMode、srcMode、numFileNames 等）进行手动标量提升：在函数入口处声明对应局部变量并初始化全局值，后续所有对这些全局变量的读取替换为局部变量读取，所有写入则同时更新局部变量和全局变量。同时，在最长文件名计算循环中缓存 strlen(aa->name) 的返回值以避免重复调用。] |
| 3 | try_flags | 1.579x  [flags: -mllvm -unroll-max-percent-threshold-boost=200] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.002x  [rewrite: 合并 kernel_bzip2_decode 中遍历 argList 的三个独立循环（计算最长文件名、解析短标志、解析长标志）为单次遍历，在循环内通过状态变量完成所有名字长度统计、模式确定和标志处理；同时提前加载 aa->name 首字符进行快速前缀判断，避免多次 strlen 或 strncmp 调用。] |
| 6 | try_flags | 1.401x  [flags: -mllvm -pragma-unroll-threshold=1000] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.115x  [rewrite: 在kernel_bzip2_decode函数定义中，将参数argv的类型从Char *argv[]改为Char * restrict * restrict argv，以向编译器提供明确的非别名保证，帮助LICM和别名分析在相关循环中提升更多不变量load，减少冗余内存访问。] |
| 9 | try_pragma | 1.503x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>1.0150x</b>（基线 86.06 ms，hash 校验，7/7 次为正）</summary>

- 任务 `c3_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0114x　最终确认：**1.0150x**　IQR [1.0113, 1.0213]　base_cv=0.2% best_cv=0.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=300 -mllvm -licm-max-num-uses-traversed=16 -mllvm -licm-max-num-int-reassociations=8`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.011x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 2 | rewrite_source | 1.006x  [rewrite: Promote global variables to local temporaries inside loops that repeatedly read or write them (e.g., for (aa = argList; …)). Specifically: (1) In the first name-length counting loop, cache longestFileName and numFileNames as locals; (2) In the flag-parsing loop, cache opMode, srcMode, blockSize100k, verbosity, smallMode, forceOverwrite, keepInputFiles into locals at loop entry, o |
| 3 | try_flags | 1.011x  [flags: -mllvm -partial-unrolling-threshold=300 -mllvm -licm-max-num-uses-traversed=16 -mllvm -licm-max-num-int-reassociations=8] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 0.999x  [rewrite: In the first loop that traverses the argList linked list to compute longestFileName (the loop containing 'numFileNames++'), cache the result of strlen(aa->name) in a local variable before the conditional comparison, so that only one call is made per iteration. Also cache aa->name[0] in a local char to avoid repeated indirect loads. This transformation is semantically equivalent a |
| 6 | try_flags | 1.007x  [flags: -mllvm --slp-min-reg-size=-1] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.006x  [rewrite: Transform the linked-list traversal of argList into array-based iteration. Allocate a local array of `Cell*` on the stack with size 256, traverse argList once to fill it, then replace all subsequent `for (aa = argList; aa != NULL; aa = aa->link)` loops with a for loop over the array. This reduces pointer chasing and improves cache locality for multiple traversals.] |
| 9 | try_flags | 1.010x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm -licm-max-num-int-reassociations=12 -mllvm -vectorize-memory-check-threshold=32] |

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0382x</b>（基线 26791.07 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb016`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0518x　最终确认：**1.0382x**　IQR [1.0374, 1.0549]　base_cv=0.8% best_cv=0.5%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=16`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.052x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=16] |
| 2 | rewrite_source | 0.998x  [rewrite: 将最内层k循环中的A[i][j]更新改为局部标量sum累加，循环结束后一次性写回并除以A[j][j]；同时将A[j][j]的加载提前到j循环入口处存入局部变量ajj，除法使用ajj。此变换消除A[i][j]在k循环内的反复写回和A[j][j]的重复加载，打破别名猜疑，改善指令调度，不改浮点累加顺序，数值一致。] |
| 3 | try_pragma | 0.999x  [pragma: #pragma clang loop unroll_count(4); #pragma clang loop unroll_count(4)] |
| 4 | try_flags | 1.048x  [flags: -mllvm -partial-unrolling-threshold=600 -mllvm -licm-max-num-uses-traversed=32] |
| 5 | rewrite_source | 1.009x  [rewrite: Add 'restrict' qualifier to the pointer parameter A in function signature: change 'DATA_TYPE POLYBENCH_2D(A,N,N,n,n)' to 'DATA_TYPE (* restrict A)[N]' (or equivalent). This tells the compiler that A does not alias any other memory, allowing LICM to hoist previously missed loop‑invariant loads and enabling the vectorizer to reorder floating‑point operations without violating stric |
| 6 | try_flags | 1.002x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 7 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.00e-02 at index 64 (ref=0.92, opt=0.93), epsilon=1.00e-04 |
| 8 | rewrite_source | 1.010x  [rewrite: 对i循环内部的j循环（遍历0到i-1）进行cache blocking：引入块大小T=64，将j循环分裂为外层分块循环和内层原始j循环。原始的j循环和k循环保持不变，仅在外层包裹一个步长为T的分块循环。此变换使得在每一个j块内，A[i][k]（行i）被频繁重用而不被后续j所使用的其他行逐出L1 cache，降低容量缺失，且所有浮点操作的次序和操作数完全相同，保证数值一致。] |
| 9 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=16] |

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>1.0045x</b>（基线 2.43 ms，hash 校验，30/51 次为正）</summary>

- 任务 `c3_cb007`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1402x　最终确认：**1.0045x**　IQR [0.9845, 1.0225]　base_cv=20.2% best_cv=19.3%
- 最终采纳编译选项：`-mllvm -vectorize-memory-check-threshold=256`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.095x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 2 | rewrite_source | 1.140x  [rewrite(utils/TIFFWriteScanline): 将 TIFFWriteScanline 中多次访问的 td->td_imagelength、td->td_planarconfig、td->td_rowsperstrip、td->td_stripsperimage、td->td_samplesperpixel 等成员缓存到局部 register 变量，避免重复解引用；将 row/td->td_rowsperstrip 的计算提前并复用；将 td->td_planarconfig == PLANARCONFIG_SEPARATE 结果缓存，减少重复比较。] |
| 3 | try_flags | 1.112x  [flags: -mllvm -slp-max-reg-size=256 -mllvm -licm-max-num-uses-traversed=16 -mllvm -slp-max-look-ahead-depth=3] |
| 4 | try_pragma | 0.894x  [pragma: #pragma clang loop vectorize(enable); #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.030x  [rewrite(utils/TIFFWriteScanline): 在TIFFWriteScanline函数开头，将tif->tif_row, tif->tif_curstrip, tif->tif_flags, tif->tif_rawcp以及四个函数指针(tif_setupencode, tif_preencode, tif_seek, tif_encoderow)缓存到局部register变量，并在函数内一致使用局部变量，最后将可能被修改的值写回tif结构体。这样可以消除重复的tif指针解引用和间接调用开销。] |
| 6 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 7 | rewrite_source | 1.013x  [rewrite(utils/TIFFWriteScanline): 在TIFFWriteScanline函数中，将 strip % stripsperimage 的计算结果缓存到一个局部变量 strip_mod，并重用该变量来计算 tif->tif_row，避免在代码中重复执行模运算。同时缓存只读的 td->td_nstrips 到局部变量，减少一次指针解引用。] |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>1.0212x</b>（基线 2.37 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c3_cb008`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2768x　最终确认：**1.0212x**　IQR [0.9704, 1.1546]　base_cv=14.2% best_cv=14.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.783x  [rewrite(utils/TIFFWriteDirectory): 对字节交换循环进行循环展开，每次处理4个 TIFFDirEntry，将 TIFFSwabArrayOfShort 和 TIFFSwabArrayOfLong 替换为内联的字节交换操作（使用局部变量批量交换），消除函数调用开销并减少循环迭代次数。] |
| 3 | try_pragma | 1.277x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpo_0hwdmu/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp4u8o6s4n/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_pragma | 1.002x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>0.9945x</b>（基线 1.23 ms，hash 校验，19/51 次为正，⚠ 正确性门无效）</summary>

- 任务 `c3_cb009`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3275x　最终确认：**0.9945x**　IQR [0.9341, 1.0146]　base_cv=37.7% best_cv=38.3%
- 最终采纳编译选项：`-mllvm --pragma-unroll-full-max-iterations=256`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.092x  [flags: -mllvm -slp-max-reg-size=256] |
| 2 | rewrite_source | 0.568x  [rewrite(utils/TIFFReadDirectory): 在第一个只提取 SamplesPerPixel 的循环之后，对目录条目数组 dir 按 tdir_tag 排序（使用 qsort），然后移除后续两个主循环中的无序标签警告和线性 while 查找，改为基于有序扫描的 fix 单调递增；同时将 tif->tif_fieldinfo 和 tif->tif_nfields 提升为局部常量指针和整数，以减少指针间接并消除 LICM 的别名障碍。] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.328x  [flags: -mllvm --pragma-unroll-full-max-iterations=256] |
| 5 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=6d49fda21902, opt=0dfbf8ecb1fd) |
| 6 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] optimized version returned non-zero exit code -11 |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | try_flags | 1.175x  [flags: -mllvm -slp-max-reg-size=256 -mllvm -pragma-unroll-full-max-iterations=512] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>0.9984x</b>（基线 3.01 ms，hash 校验，23/51 次为正）</summary>

- 任务 `c3_cb010`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0751x　最终确认：**0.9984x**　IQR [0.9931, 1.0080]　base_cv=3.0% best_cv=3.0%
- 最终采纳编译选项：`-mllvm --licm-mssa-max-acc-promotion=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.075x  [flags: -mllvm --licm-mssa-max-acc-promotion=64] |
| 2 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpvvfx25je/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 3 | try_pragma | 0.996x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | rewrite_source | 0.996x  [rewrite(utils/TIFFWriteDirectory): 针对末尾字节交换循环 (for (dir = (TIFFDirEntry*) data; dircount; dir++, dircount--))，将内部的 TIFFSwabArrayOfShort 和 TIFFSwabArrayOfLong 调用替换为直接的字节交换内建函数 (__builtin_bswap16 和 __builtin_bswap32)，从而使编译器能够自动向量化该循环，消除函数调用开销并利用 SIMD 指令加速字节序转换。] |
| 5 | try_flags | 1.000x  [flags: -mllvm --licm-mssa-max-acc-promotion=64] |
| 6 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpqvszvscd/rw_utils_utils_shadow/polybench.c:1355:8: note: add parentheses after the '!' to evaluate the comparison first |
| 7 | try_flags | 1.060x  [flags: -mllvm -vectorize-memory-check-threshold=12 -mllvm -licm-max-num-uses-traversed=32] |
| 8 | try_pragma | 1.034x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.000x  [flags: -mllvm --licm-mssa-max-acc-promotion=64] |

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>11.3003x</b>（基线 4896.50 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c3_pb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：11.3828x　最终确认：**11.3003x**　IQR [11.2991, 11.4320]　base_cv=0.5% best_cv=0.3%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.003x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 2 | rewrite_source | 11.316x  [rewrite: 将相关性计算的三重循环 for (i=0;i<M-1;i++) for (j=i+1;j<M;j++) for (k=0;k<N;k++) 变换为 for (i=0;i<M-1;i++) for (k=0;k<N;k++) { double tmp=data[k][i]; for (j=i+1;j<M;j++) corr[i][j]+=tmp*data[k][j]; }，使内层 j 循环访问 data[k][j] 连续，提升 data[k][i] 为不变量，提高 cache 局部性和向量化机会。] |
| 3 | try_flags | 11.231x  [flags: -mllvm -licm-mssa-max-acc-promotion=4] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 11.186x  [rewrite: 在 i 循环内分配标量累加器数组 acc[0..M-1] 并初始化为 0.0；将 k 循环内的 j 循环改写为 acc[j] += tmp * data[k][j]；在 k 循环结束后将 acc[j] 写入 corr[i][j] 和 corr[j][i]。此举消除对内层 j 循环中全局数组 corr 的写后读依赖，促进向量化。] |
| 6 | try_flags | 11.383x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 11.167x  [rewrite: 在相关性计算的内层 j 循环中，将其按步长 4 手动展开：对于每个 k，保持 tmp = data[k][i] 不变，将 for (j=i+1; j<_PB_M; j++) corr[i][j] += tmp * data[k][j] 改写为 for (j=i+1; j<_PB_M; j+=4) { corr[i][j] += tmp * data[k][j]; corr[i][j+1] += tmp * data[k][j+1]; corr[i][j+2] += tmp * data[k][j+2]; corr[i][j+3] += tmp * data[k][j+3]; } 加上剩余尾循环。此变换显式将连续 4 个独立乘加操作暴露为标量语句，帮助 SLP 将其打包为 4×double 的 SIMD 向量指令，同时避免引入额外的累加器数组开销。] |
| 9 | try_flags | 11.304x  [flags: -mllvm --earlycse-mssa-optimization-cap=8] |

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>16.3732x</b>（基线 4894.65 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb002`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：16.2374x　最终确认：**16.3732x**　IQR [16.3120, 16.4838]　base_cv=0.8% best_cv=0.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.013x  [flags: -mllvm -partial-unrolling-threshold=150 -mllvm -vectorizer-min-trip-count=1 -mllvm -vectorize-memory-check-threshold=8] |
| 2 | rewrite_source | 13.891x  [rewrite: Loop interchange on the third nest (covariance computation): promote loop k (0.._PB_N) to outermost, keep i (0.._PB_M) and j (i.._PB_M) as inner loops in that order. This changes 'data[k][i]' and 'data[k][j]' from strided (row‑major) to contiguous access along the innermost dimension, drastically improving cache locality and enabling auto‑vectorization. The final reduction and s |
| 3 | try_flags | 13.949x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 16.010x  [rewrite: 对 cov 矩阵进行 i 维度分块（block size B=32）。在外层 k 循环前增加 i 分块循环，每次处理一块 i 行，分配局部数组 block_cov[B][M] 并零初始化。在 k 循环中只对当前 i 块执行 j 循环，将累加写入 block_cov。所有 k 完成后，将 block_cov 写回 cov。最终消除每次 k 迭代对 cov 的加载存储。] |
| 6 | try_flags | 15.985x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 16.125x  [rewrite: Swap the loop order of the subtraction nest (currently `for i` outer, `for j` inner) to `for j` outer, `for i` inner: `for (j = 0; j < _PB_M; j++) for (i = 0; i < _PB_N; i++) data[i][j] -= mean[j];`. This makes `data[i][j]` access contiguous (row‑major order) and holds `mean[j]` constant in the innermost loop, improving cache reuse and allowing better auto‑vectorisation.] |
| 9 | try_flags | 16.237x  [flags: -mllvm -partial-unrolling-threshold=200] |

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>2.0760x</b>（基线 228.15 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb022`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：2.2565x　最终确认：**2.0760x**　IQR [2.0172, 2.0857]　base_cv=0.5% best_cv=1.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.866x  [rewrite: Apply loop tiling to the outer loops: tile the i-loop of the horizontal pass (first three loops) with block size BS=64, and tile the j-loop of the vertical pass (next three loops) with BS=64. Also add restrict qualifiers to the source arrays (imgIn, imgOut, y1, y2) to enable SLP vectorization of the fusion loops. Keep inner loop order unchanged to preserve recurrence semantics.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 2.256x  [rewrite: 交换垂直 pass 循环顺序：将外循环 j（列）与内循环 i（行）交换，对递归状态 (tm1,ym1,ym2 及 tp1,tp2,yp1,yp2) 使用大小 _PB_H 的标量数组保存逐列历史，使 y1/y2/imgOut 的全部访问变为行优先连续访问。同时将垂直 pass 最后的元素级融合循环也改为内层 j 的连续访问形态。水平 pass 保持不变。] |
| 6 | try_flags | 1.927x  [flags: -mllvm -slp-max-look-ahead-depth=3] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.819x  [rewrite: 合并遍历：将垂直pass最后的倒序循环（i从_W-1到0）与融合循环 imgOut=c1*(y1+y2) 合并，在该循环末尾写入imgOut，消除原独立融合循环；将水平pass倒序扫描循环（j从_H-1到0）与最后的融合循环 imgOut=c2*(y1+y2) 合并，同样在该循环末尾写入imgOut，消除最终独立融合循环。这样整个kernel减少两趟全数组遍历，降低内存带宽压力。] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>4.8992x</b>（基线 557.28 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb014`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：4.8986x　最终确认：**4.8992x**　IQR [4.8965, 4.9238]　base_cv=0.3% best_cv=0.2%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=32`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.087x  [flags: -mllvm --partial-unrolling-threshold=128] |
| 2 | rewrite_source | 4.615x  [rewrite: Replace the temporary array sum[NP] with a local scalar accumulator 'acc' inside the p-loop: for each (r,q), compute 'acc = 0.0; for (s) acc += A[r][q][s] * C4[s][p]; then A[r][q][p] = acc;' This eliminates all loads/stores to sum[] while preserving the exact summation order.] |
| 3 | try_flags | 4.765x  [flags: -mllvm -partial-unrolling-threshold=64] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 4.721x  [rewrite: Add __restrict qualifiers to the function pointer parameters A, C4, and sum in the function signature. This tells the compiler that these arrays do not alias each other, removing barriers to loop vectorization, LICM, and instruction reordering. The transformation preserves all numerical results because the original specification assumes no aliasing among the input/output buffers. |
| 6 | try_flags | 4.894x  [flags: -mllvm --partial-unrolling-threshold=32] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 3.679x  [rewrite: 对 q 循环进行 cache blocking（分块），在 q 维度引入块大小 BQ（例如 32），将原本 for q=0..NQ-1 循环改为外层层按 BQ 步进，内层处理块内的 q，使得固定块内多个 q 共享 C4 矩阵，显著提高 C4 的数据复用并减少缓存未命中。] |
| 9 | try_flags | 4.899x  [flags: -mllvm --partial-unrolling-threshold=32] |

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.3223x</b>（基线 3.45 ms，numeric 校验，51/51 次为正）</summary>

- 任务 `c3_pb017`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.7578x　最终确认：**1.3223x**　IQR [1.3101, 1.3348]　base_cv=2.7% best_cv=3.6%
- 最终采纳编译选项：`-mllvm -slp-threshold=-8`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.108x  [flags: -mllvm -slp-threshold=10] |
| 2 | rewrite_source | 1.471x  [rewrite: Eliminate temporary array z by performing in-place symmetric update of y: for i from 0 to (k-1)/2, compute new_y_i = y[i] + alpha*y[k-i-1] and new_y_j = y[k-i-1] + alpha*y[i] using saved old values, then write both back. Handle middle element when k is even separately. Also manually unroll the sum reduction loop by 4 to increase ILP.] |
| 3 | try_flags | 1.684x  [flags: -mllvm -slp-threshold=-8] |
| 4 | try_pragma | 1.758x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.344x  [rewrite: Add restrict qualifiers to local pointers of arrays r and y to inform the compiler that they do not alias, potentially enabling better load elimination, vectorization, and reducing register spills.] |
| 6 | try_flags | 1.442x  [flags: -mllvm --slp-max-stride=8] |
| 7 | rewrite_source | 1.039x  [rewrite: Manually unroll the in-place symmetric update loop by 4 (processing four symmetric element pairs per iteration) and add #pragma clang loop vectorize(enable) to force vectorization. Use local accumulators for the update to reduce address calculations and improve SIMD width.] |
| 8 | try_pragma | 1.314x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.476x  [flags: -mllvm --slp-max-reg-size=-4] |

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>1.1294x</b>（基线 927.54 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb026`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1788x　最终确认：**1.1294x**　IQR [1.1152, 1.1329]　base_cv=0.2% best_cv=0.8%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=500`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.002x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 2 | rewrite_source | 1.034x  [rewrite: Fuse the first two inner update loops (ey[i][j] and ex[i][j]) into a single nested loop over i=0..NX-1, j=0..NY-1 using conditional updates to safely combine the ranges (i>=1 for ey, j>=1 for ex), thereby reducing one full traversal of the hz array per time step.] |
| 3 | try_flags | 1.126x  [flags: -mllvm -partial-unrolling-threshold=500] |
| 4 | try_pragma | 1.177x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.160x  [rewrite: Apply overlapping i-tiling inside each time step: tile the i-dimension with block size B (e.g., 128). In each block, first compute ex and ey for rows i = start to min(start+B+1, NX)-1 (one extra row as ghost), then compute hz for rows i = start to min(start+B, NX-1)-1. This ensures hz updates immediately consume the ex/ey values while they are still in cache, improving spatial an |
| 6 | try_flags | 1.122x  [flags: -mllvm -partial-unrolling-threshold=600 -mllvm -slp-max-reg-size=512] |
| 7 | rewrite_source | 0.957x  [rewrite: 对融合ex/ey更新循环的内层j循环和hz更新循环的内层j循环同时进行手动展开（unroll factor 4），使用标量寄存器变量显式重复迭代体，减少循环分支开销并扩大SLP向量化窗口，同时保持所有数组访问顺序不变以确保依赖正确。] |
| 8 | try_pragma | 1.179x  [pragma: #pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)] |
| 9 | try_flags | 1.096x  [flags: -mllvm --partial-unrolling-threshold=128 -mllvm -unroll-threshold=1500 -mllvm --licm-mssa-max-acc-promotion=256] |

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>5.6858x</b>（基线 15851.00 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c3_pb023`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：5.8513x　最终确认：**5.6858x**　IQR [5.6697, 5.7380]　base_cv=0.0% best_cv=0.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.009x  [rewrite: Inside the k-loop, before the i-loop, tile i into blocks of size 128. For each i-block, load path[k][j] row once into a local array (or rely on cache reuse by the inner loops). Inside the i-block loop, for each i, load path[i][k] into a scalar variable ik outside the j-loop, then run the original j-loop using ik. Additionally, preload the entire row path[k][j] into a local buffer |
| 3 | try_flags | 1.009x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 4 | try_pragma | 1.009x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 5.754x  [rewrite: Tiling on the i-loop with block size 64. Inside the k-loop, before the i-loop, preload the row path[k][j] for all j into a local array kj_row[N]. Then, tile the i-loop in blocks of 64: for each i-block, for each i in the block, load ik = path[i][k] into a scalar, and run the original j-loop using ik + kj_row[j] instead of ik + path[k][j]. Keep the existing #pragma clang loop vect |
| 6 | try_flags | 5.670x  [flags: -mllvm -licm-mssa-max-acc-promotion=4] |
| 7 | rewrite_source | 5.340x  [rewrite: On top of the existing i-tiling (block size 64) and k-row preloading, also tile the j-loop with block size 64. Inside each i-block, split the j-loop into blocks of 64: for each j-block, run the inner i/j loops over that tile, computing path[i][j] = min(path[i][j], ik + kj_row[j]). This creates a 64x64 tile working set (~16KB) that fits in L1d, improving spatial locality for both  |
| 8 | try_pragma | 5.637x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 5.851x  [flags: -mllvm -partial-unrolling-threshold=64] |

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>1.0551x</b>（基线 290.47 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb003`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0544x　最终确认：**1.0551x**　IQR [1.0518, 1.0554]　base_cv=0.3% best_cv=0.2%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=400 -mllvm --unroll-optsize-threshold=64 -mllvm -licm-versioning-invariant-threshold=0.25`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.054x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm --unroll-optsize-threshold=64 -mllvm -licm-versioning-invariant-threshold=0.25] |
| 2 | rewrite_source | 0.998x  [rewrite: Transform loops from i-k-j to i-j-k order: for i, for j, compute C[i][j] *= beta, then use a scalar accumulator to sum alpha*A[i][k]*B[k][j] over k, and store final result back to C[i][j]. This eliminates repeated C reads/writes inside the k loop and improves locality.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm --unroll-optsize-threshold=64 -mllvm -licm-versioning-invariant-threshold=0.25] |
| 5 | rewrite_source | 0.522x  [rewrite: 对 j 循环进行分块（tile size 例如 256），在内层插入标量累加器数组部分存储 C 结果，保持 i‑k‑j 循环顺序，从而减少对 C 数组的重复读取和写回，提高缓存重用。] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm --unroll-optsize-threshold=64 -mllvm -licm-versioning-invariant-threshold=0.25] |
| 8 | rewrite_source | 0.998x  [rewrite: Inside the i-loop, allocate a local stack array tmp[NJ] (initialized to zero). In the k-loop, for j from 0 to NJ-1, compute tmp[j] += A[i][k] * B[k][j]. After the k-loop, for j from 0 to NJ-1, update C[i][j] = C[i][j]*beta + alpha * tmp[j] (the beta scaling of C is done first). This eliminates repeated C reads/writes in the k-loop, breaks the aliasing that blocks LICM and GVN, an |
| 9 | try_flags | 1.053x  [flags: -mllvm -partial-unrolling-threshold=300] |

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.5221x</b>（基线 37.64 ms，numeric 校验，15/15 次为正）</summary>

- 任务 `c3_pb004`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.5314x　最终确认：**1.5221x**　IQR [1.4937, 1.5267]　base_cv=0.9% best_cv=1.5%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.026x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 2 | rewrite_source | 1.515x  [rewrite: Tile the second loop nest (i outer, j inner) that computes x[i] += beta*A[j][i]*y[j] with tile sizes of 64 in both i and j dimensions. Inside the inner tile, accumulate into a local scalar 'sum' before writing back to x[i], reducing store traffic.] |
| 3 | try_flags | 1.371x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.531x  [rewrite: In the fourth loop nest (i outer, j inner) that computes w[i] += alpha * A[i][j] * x[j], promote w[i] to a local scalar accumulator: initialize 'sum = w[i]', accumulate alpha*A[i][j]*x[j] into 'sum' in the inner j loop, and store 'sum' back to w[i] after the j loop. This eliminates redundant store traffic to w[i] inside the inner loop.] |
| 6 | try_flags | 1.508x  [flags: -mllvm -vectorize-num-stores-pred=2] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.470x  [rewrite: Split the first loop (A[i][j] += u1[i]*v1[j] + u2[i]*v2[j]) into two separate loops: first do A[i][j] += u1[i]*v1[j], then do A[i][j] += u2[i]*v2[j]. This simplifies each loop to a single multiply-add, improving vectorization potential.] |
| 9 | try_flags | 1.509x  [flags: -mllvm -licm-max-num-uses-traversed=128] |

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>1.3317x</b>（基线 22.68 ms，numeric 校验，25/25 次为正）</summary>

- 任务 `c3_pb005`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3638x　最终确认：**1.3317x**　IQR [1.2828, 1.3571]　base_cv=2.3% best_cv=2.7%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200 -mllvm -vector-combine-max-scan-instrs=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.147x  [flags: -mllvm -vectorize-memory-check-threshold=0 -mllvm -earlycse-mssa-optimization-cap=1 -mllvm --slp-max-vf=8] |
| 2 | rewrite_source | 1.295x  [rewrite: 引入标量累加器消除内层循环中对 tmp[i] 和 y[i] 的冗余 load/store：在 i 循环内声明 double tmp_acc=0.0, y_acc=0.0；j 循环内对两个累加器做 fma；循环结束后写入 tmp[i]=tmp_acc, y[i]=alpha*tmp_acc+beta*y_acc] |
| 3 | try_flags | 1.364x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -vector-combine-max-scan-instrs=200] |
| 4 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.68e-04 at index 74 (ref=59.48, opt=59.47), epsilon=1.00e-04 |
| 5 | rewrite_source | 1.342x  [rewrite: 将内层 j 循环拆分为两个独立的 j 循环：第一个循环累加 tmp_acc = A[i][j]*x[j]; 第二个循环累加 y_acc = B[i][j]*x[j]; 保持累加顺序与原始完全一致，不改变任何浮点运算顺序。] |
| 6 | try_flags | 1.304x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 7 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.68e-04 at index 74 (ref=59.48, opt=59.47), epsilon=1.00e-04 |
| 8 | rewrite_source | 1.223x  [rewrite: 在 kernel_gesummv 函数声明中为所有指针参数（A, B, tmp, x, y）添加 __restrict 限定符，以告知编译器这些指针不与其他任何指针别名，从而消除别名分析障碍，允许 LICM 提升循环不变地址的 load，并可能使 LoopVectorizePass 启动向量化生成 SIMD 代码。] |
| 9 | try_flags | 1.304x  [flags: -mllvm -partial-unrolling-threshold=250] |

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>14.6265x</b>（基线 6969.57 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb018`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：14.8642x　最终确认：**14.6265x**　IQR [14.4137, 14.8196]　base_cv=1.5% best_cv=0.1%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 14.250x  [rewrite: 在k外层循环内，重构j循环部分：将原来的 for(j=k+1..N-1) { for(i=0..M) { ... } } 交换为 for(i=0..M) { for(j=k+1..N-1) { ... } }。具体：先单独清零R[k][j]；然后以i为外层、j为内层累加 R[k][j] += Q[i][k] * A[i][j]，同时将Q[i][k]提升到i循环外为标量qik；最后用相同的i-j嵌套做更新 A[i][j] -= qik * R[k][j]。此变换将内层跨步访问转换为j下标连续访问，显著提升缓存局部性和向量化机会。] |
| 3 | try_flags | 8.731x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 14.570x  [rewrite: 在 kernel_gramschmidt 函数开头，将传入指针 A, R, Q 重新绑定到 restrict 局部指针（例如 DATA_TYPE (* restrict A_loc)[1200] = A;），并在所有循环体中使用这些局部指针代替原指针。这向编译器声明不同数组彼此不重叠，消除别名分析障碍，允许 LICM 将 Q[i][k] 提升至内层j循环之外，并降低 LoopVectorizePass 的内存检查开销，从而可能触发内层j循环的 SIMD 向量化。] |
| 6 | try_flags | 10.073x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 14.864x  [rewrite: 在 current_best 源码的基础上，为无浮点归约依赖的循环手动添加向量化 pragma：(1) 对 Q[i][k] 列计算循环 for (i=0; i<_PB_M; ++i) 前添加 #pragma clang loop vectorize(enable)；(2) 对 A 更新循环的最内层 j 循环 for (j=k+1; j<_PB_N; ++j) 前添加 #pragma clang loop vectorize(enable) 和 #pragma clang loop interleave(enable) (因为该循环内元素独立，向量化安全且受益于连续访存)；不对含归约的 R 累加循环和 nrm 计算循环添加 pragma，以确保数值完全一致。] |
| 9 | try_flags | 8.774x  [flags: -mllvm -slp-threshold=0] |

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.0451x</b>（基线 2331.88 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c3_pb027`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0481x　最终确认：**1.0451x**　IQR [1.0435, 1.0470]　base_cv=0.1% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=100`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.004x  [flags: -mllvm -slp-max-look-ahead-depth=2] |
| 2 | rewrite_source | 1.013x  [rewrite: Apply loop tiling to the i and j loops of both time-step stencil blocks. Block size chosen to keep a tile of A and B (e.g., 32×32×120) in L2/L3 cache, reducing main-memory traffic and improving temporal reuse of stencil neighbors across the innermost k loop. Keep k as the innermost loop for unit-stride access.] |
| 3 | try_flags | 1.048x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.031x  [rewrite: Inside the k loop of both half-steps, load A[i][j][k] (resp. B[i][j][k]) into a local DATA_TYPE scalar once, then use that scalar throughout the stencil expression instead of re-accessing the array.] |
| 6 | try_flags | 1.016x  [flags: -mllvm -slp-max-reg-size=128] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.018x  [rewrite: 在 kernel_heat_3d 函数的两个半步骤中，在两个最内层 `for (k = 1; k < _PB_N-1; k++)` 循环前分别插入 `#pragma clang loop vectorize(enable) vectorize_width(4) interleave(enable) interleave_count(2)`，强制编译器使用 4-wide 向量化和交错执行，在不改变数值结果的前提下利用更宽的 SIMD 通路。] |
| 9 | try_flags | 1.015x  [flags: -mllvm -vectorize-memory-check-threshold=8] |

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>0.8333x</b>（基线 1.52 ms，numeric 校验，0/51 次为正）</summary>

- 任务 `c3_pb028`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0161x　最终确认：**0.8333x**　IQR [0.8299, 0.8375]　base_cv=2.8% best_cv=2.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.872x  [rewrite: Apply time tiling (loop interchange) to the Jacobi iteration: partition the inner spatial range 1..PB_N-2 into chunks of size TILE (e.g., 256). Loop nest becomes outer spatial-chunk loop, then an inner time-step loop that updates A and B only within the chunk for all TSTEPS. A small scratch buffer of length TILE+2 holds boundary values (A left neighbor from previous chunk) to pre |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 0.926x  [rewrite: Add __restrict qualifiers to the A and B pointer parameters in the kernel_jacobi_1d function signature, instructing the compiler that these arrays do not overlap, thus enabling more aggressive load/store elimination and vectorization.] |
| 6 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 7 | rewrite_source | 1.016x  [rewrite: Manually unroll each of the two inner loops by a factor of 2, explicitly reusing the overlapping loads (e.g., A[i+1] is shared between the update of B[i] and B[i+1]). This reduces the number of loads per element without changing the memory access order.] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>1.1751x</b>（基线 1126.85 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb029`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1773x　最终确认：**1.1751x**　IQR [1.1725, 1.1765]　base_cv=0.1% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=500 -mllvm -slp-max-vf=4 -mllvm -dse-memoryssa-defs-per-block-limit=100`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.176x  [flags: -mllvm -partial-unrolling-threshold=500] |
| 2 | rewrite_source | 0.958x  [rewrite: Apply i-loop tiling (tile width 256) inside each timestep's B and A computation loops to improve temporal cache locality. Keep the j-loop inner, and tile the i-loop for both stencil computations separately. Also extract common subexpressions into scalar variables to aid SLP vectorization.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.177x  [flags: -mllvm -partial-unrolling-threshold=500 -mllvm -slp-max-vf=4 -mllvm -dse-memoryssa-defs-per-block-limit=100] |
| 5 | rewrite_source | 1.027x  [rewrite: Apply 2D cache blocking (tiling) to both stencil loops: tile the i dimension with tile size 64 and the j dimension with tile size 64, using min() for boundary tiles. Keep the original computation inside the tile loops unchanged. This restricts the working set of each tile to ~64×64 doubles (≈32KB per array, well within L1d), improving temporal locality and reducing off-chip memor |
| 6 | try_flags | 1.003x  [flags: -mllvm -slp-max-look-ahead-depth=1] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.013x  [rewrite: Add __restrict qualifier to the pointer parameters of kernel_jacobi_2d (i.e., change the function signature to void kernel_jacobi_2d(int tsteps, int n, DATA_TYPE POLYBENCH_2D(A,N,N,n,n) __restrict, DATA_TYPE POLYBENCH_2D(B,N,N,n,n) __restrict)). This informs the compiler that A and B do not alias, allowing GVN to eliminate clobbered loads and potentially improving SLP vectorizati |
| 9 | try_flags | 1.003x  [flags: -mllvm -slp-max-vf=16 -mllvm -slp-max-look-ahead-depth=8] |

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.0429x</b>（基线 32146.35 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb020`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0271x　最终确认：**1.0429x**　IQR [1.0118, 1.0469]　base_cv=1.7% best_cv=0.3%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.981x  [rewrite: Introduce scalar replacement for A[i][j] and manual hoisting of A[j][j] in both inner loops. For the first inner loop (j < i): load double ajj = A[j][j] before the k loop and accumulate into a local double sum = A[i][j]; then after the k loop store A[i][j] = sum / ajj. For the second inner loop (j >= i): use a local accumulator sum = A[i][j], subtract contributions in the k loop, |
| 3 | try_pragma | 1.005x  [pragma: #pragma clang loop vectorize(enable); #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 1.003x  [rewrite: Apply cache blocking (tiling) on the j dimension of both inner loops. Introduce a TILE_SIZE constant (e.g., 64). For each i, iterate over jj from 0 to _PB_N in steps of TILE_SIZE, and inside each tile run: (1) for j from jj to min(jj+TILE_SIZE, i) execute the original first inner loop (j < i) with k from 0 to j; (2) for j from max(jj, i) to min(jj+TILE_SIZE, _PB_N) execute the or |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 1.027x  [rewrite: Apply two-level tiling on i and j loops: for (ii=0; ii<_PB_N; ii+=BI) for (jj=0; jj<_PB_N; jj+=BJ) process the tile A(ii:min(ii+BI,_PB_N), jj:min(jj+BJ,_PB_N)) using the original Doolittle algorithm but with a temporary scratch buffer 'double sum' that accumulates the dot product in the k loop and is written back to A[i][j] once after the k loop. Also hoist A[j][j] into a local s |
| 8 | try_pragma | 0.996x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.000x  [flags: -mllvm -licm-mssa-max-acc-promotion=64] |

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.1288x</b>（基线 53292.49 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb019`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1165x　最终确认：**1.1288x**　IQR [1.1196, 1.1336]　base_cv=0.6% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -vectorize-memory-check-threshold=16 -mllvm -vectorize-scev-check-threshold=16 -mllvm -partial-unrolling-threshold=600`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.998x  [rewrite: Add '__restrict' qualifier to the matrix and vector pointer parameters (A, b, x, y) in the function signature to inform the compiler that these arrays do not overlap and are not aliased, enabling loop vectorization of the hot w‑=A[i][k]*A[k][j] loops.] |
| 3 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp_98o55ql/ludcmp_rewrite.c:99:1: error: expected function body after function declarator |
| 6 | rewrite_source | 1.001x  [rewrite: Implement a blocked LU decomposition (blocksize B = 64). Keep the original kernel interface and ensure numerical equivalence. The algorithm: (1) For each block column jj from 1 to N with stride B, first factorize the diagonal block A[jj:jj+B-1][jj:jj+B-1] using the original unblocked LU (column‑wise operations to preserve exact dependency order). (2) Apply row swaps implied by th |
| 7 | try_pragma | 0.975x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=16 -mllvm -vectorize-scev-check-threshold=16 -mllvm -partial-unrolling-threshold=600] |
| 9 | rewrite_source | 1.117x  [rewrite: Adjust block size to 128 (to fully utilise L1 cache) and apply register‑tiling to the inner k‑loop of the trailing‑submatrix update (the DGEMM‑like part). Explicitly strip‑mine the k loop with a step of 4 and unroll the body into four independent multiply‑accumulate statements (w -= A[i][k+0]*A[k+0][j]; w -= A[i][k+1]*A[k+1][j]; w -= A[i][k+2]*A[k+2][j]; w -= A[i][k+3]*A[k+3][j]) |

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>1.6580x</b>（基线 33.72 ms，numeric 校验，15/15 次为正）</summary>

- 任务 `c3_pb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.6172x　最终确认：**1.6580x**　IQR [1.6373, 1.6885]　base_cv=1.4% best_cv=2.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.617x  [rewrite: Interchange loops in the second reduction (swap i and j) to make A access consecutive, enabling vectorization. Use a local scalar accumulator for the x1[i] reduction in the first loop to reduce store-load forwarding overhead.] |
| 3 | try_flags | 1.493x  [flags: -mllvm -vectorize-scev-check-threshold=500] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.552x  [rewrite: For the second loop (x2 reduction), wrap x2[i] in a local scalar accumulator (like the first loop), accumulating all contributions into a temp var before writing back to x2[i] once. This avoids repeated loads and stores to x2[i] during the inner loop, reducing store-load forwarding stalls without altering the floating-point reduction order.] |
| 6 | try_flags | 1.481x  [flags: -mllvm -licm-max-num-uses-traversed=8] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.518x  [rewrite: In the second reduction loop (for j outer, i inner), manually unroll the i-loop with step 4: for (i = 0; i < N-3; i+=4) { x2[i] += A[j][i]*yj; x2[i+1] += A[j][i+1]*yj; x2[i+2] += A[j][i+2]*yj; x2[i+3] += A[j][i+3]*yj; } followed by cleanup for remaining i. This keeps the sequential update order unchanged but exposes four independent contiguous updates per iteration to guide the v |
| 9 | try_flags | 1.465x  [flags: -mllvm --unroll-threshold=4] |

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>0.9968x</b>（基线 0.99 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c3_cb011`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.4392x　最终确认：**0.9968x**　IQR [0.9252, 1.1074]　base_cv=15.5% best_cv=15.2%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.620x  [rewrite: Apply cache blocking (loop tiling) to the inner distance-update loop (for j=0..NUM_NODES) in dijkstra: introduce a tile size B (e.g., 128) and process nodes in blocks. For each tile, preload the tile of rgnNodes and Dist into local stack arrays or reuse existing arrays with better cache access, then perform the distance update for the current source node i against all j in the ti |
| 3 | try_pragma | 1.439x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 1.131x  [rewrite: 在dijkstra函数开头声明两个局部restrict指针：int * restrict adj = AdjMatrix; NODE * restrict nodes = rgnNodes; 在while循环内部、for循环之前，将当前源节点的行基址计算为临时变量：int * restrict row = &adj[iNode * NUM_NODES]; 然后for循环内用row[i]替换AdjMatrix[iNode*NUM_NODES+i]，用nodes[i].iDist/nodes[i].iPrev替换rgnNodes[i]的访问。此变换彻底告知编译器adj和nodes内存区域无重叠，从而允许LICM提升循环不变地址计算并减少加载指令数，同时不改变数值结果。] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 0.841x  [rewrite: 对 dijkstra 中 while(qcount()>0) 内的 for (i=0..NUM_NODES) 距离扫描循环进行批量 enqueue 变换：在循环外声明两个局部数组 queue_nodes[128] 和 queue_dists[128]，当条件满足需要更新节点时，将 i 和新距离存入数组；数组满或循环末尾时，批量调用 enqueue。消除循环内的函数调用障碍，使编译器能够向量化整个距离扫描与更新逻辑，同时保持 Dijkstra 算法正确性。] |
| 8 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>1.0041x</b>（基线 1.55 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c3_cb012`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.8794x　最终确认：**1.0041x**　IQR [0.9923, 1.0127]　base_cv=4.4% best_cv=4.6%
- 最终采纳编译选项：`-mllvm -slp-max-stride=8 -mllvm -unswitch-threshold=100 -mllvm -simple-loop-unswitch-inject-invariant-condition-hotness-threshold=0`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.618x  [flags: -mllvm --slp-min-strided-loads=8] |
| 2 | rewrite_source | 1.099x  [rewrite: 针对主while循环：1) 将常量mask的htonl结果提升到循环外；2) 将print条件判断外移，生成print==1和print!=1两个循环版本，消除循环内分支；3) 将每次迭代的3次malloc（ptree节点、ptree_mask、MyNode）合并为一次连续内存分配，减少分配开销并改善缓存局部性。] |
| 3 | try_flags | 1.879x  [flags: -mllvm -slp-max-stride=8 -mllvm -unswitch-threshold=100 -mllvm -simple-loop-unswitch-inject-invariant-condition-hotness-threshold=0] |
| 4 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 5 | rewrite_source | 1.081x  [rewrite: 将 pat_search 和 pat_insert 的实现移至当前编译单元并标记为 static inline，在主循环中直接调用内联版本，消除函数调用开销并允许编译器跨过程优化（如 CSE、分支合并）。] |
| 6 | try_flags | 1.873x  [flags: -mllvm -jump-threading-threshold=6 -mllvm -licm-mssa-max-acc-promotion=8 -mllvm -slp-max-stride=4] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpwjgq_mi1/network_patricia_rewrite.c:130:32: error: use of undeclared identifier 'max_align_t' |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>1.0389x</b>（基线 3819.76 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c3_pb024`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1441x　最终确认：**1.0389x**　IQR [1.0338, 1.0408]　base_cv=0.3% best_cv=0.6%
- 最终采纳编译选项：`-mllvm -licm-mssa-max-acc-promotion=16`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.034x  [flags: -mllvm -licm-mssa-max-acc-promotion=16] |
| 2 | rewrite_source | 1.144x  [rewrite: Introduce a scalar accumulator for table[i][j] in the innermost k loop: load table[i][j] into a local variable before the k loop, update it with max_score inside, and store back after.]content empty, falling back to reasoning_content on attempt 1 |
| 3 | try_flags | 1.025x  [flags: -mllvm -vectorize-scev-check-threshold=16] |
| 4 | try_pragma | 1.083x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.040x  [rewrite: 对 j 循环和 k 循环进行缓存分块：新增外层 jj 循环以固定步长递增，在每一块内部将 k 循环的访问限制在该块列范围内，并通过临时数组或直接计算顺序重用 table[i][k]、table[k+1][j] 等被重复读取的元素，使活跃数据能驻留在 L1/L2 中。] |
| 6 | try_flags | 1.075x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 7 | rewrite_source | 1.038x  [rewrite: 对i和j循环进行cache tiling：将外层i循环按tile_size步长分块（从_PB_N-1向下递减），内层j循环同样按tile_size分块（从i+1向上递增）；在每个tile内，对k循环按块处理，确保对table[i][k]和table[k+1][j]的访问在块内具有更好的时间局部性。具体可引入临时b_block二维数组存储tile内的计算结果以减少主存储器的访问。tile_size设为64或128以适配L1 cache（1.3MB）。] |
| 8 | try_flags | 1.101x  [flags: -mllvm -slp-threshold=-4 -mllvm --licm-max-num-int-reassociations=128] |
| 9 | try_pragma | 1.107x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>1.0310x</b>（基线 2.50 ms，hash 校验，32/51 次为正）</summary>

- 任务 `c3_cb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：2.0935x　最终确认：**1.0310x**　IQR [0.9436, 1.1360]　base_cv=24.1% best_cv=24.9%
- 最终采纳编译选项：`-mllvm --unroll-max-upperbound=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 2.094x  [flags: -mllvm --unroll-max-upperbound=64] |
| 2 | rewrite_source | 1.074x  [rewrite: Manual loop-invariant code motion: hoist search_strings[i] to a local variable before the inner j-loop, and load find_strings[j] once at the start of each j-iteration, reusing both in strsearch2 and the conditional fprintf to eliminate redundant pointer loads.] |
| 3 | try_flags | 1.971x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 4 | try_pragma | 0.999x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.859x  [rewrite: Manual loop-invariant code motion: hoist search_strings[i] to a local variable before the inner j-loop, and load find_strings[j] into a local variable at the start of each j-iteration, reusing both in strsearch2 and the conditional fprintf to eliminate redundant pointer loads.] |
| 6 | try_flags | 1.822x  [flags: -mllvm --instcombine-maxarray-size=8] |
| 7 | try_pragma | 1.016x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.340x  [rewrite: 在current_best源码基础上，手动展开内层j循环4倍（unroll factor 4）：for( j=0; j<i2-3; j+=4) { char* f0 = find_strings[j]; char* f1 = find_strings[j+1]; char* f2 = find_strings[j+2]; char* f3 = find_strings[j+3]; here = strsearch2(si, f0); if(here) fprintf(...); ... 依次处理 } 余数循环单独处理；同时在外层i循环提升 search_strings[i] 到局部变量 si，减少每j迭代的间接访问。] |
| 9 | try_flags | 1.671x  [flags: -mllvm -licm-max-num-uses-traversed=64] |

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>1.0044x</b>（基线 1.33 ms，hash 校验，29/51 次为正）</summary>

- 任务 `c3_cb021`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3663x　最终确认：**1.0044x**　IQR [0.9479, 1.3161]　base_cv=22.0% best_cv=24.6%
- 最终采纳编译选项：`-mllvm -vectorize-scev-check-threshold=16 -mllvm -slp-max-vf=8 -mllvm -vectorize-num-stores-pred=2`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.366x  [flags: -mllvm -vectorize-scev-check-threshold=16 -mllvm -slp-max-vf=8 -mllvm -vectorize-num-stores-pred=2] |
| 2 | rewrite_source | 1.007x  [rewrite(utils/BF_encrypt): 在 BF_encrypt 函数定义中为 data 和 key 参数添加 restrict 限定符，宣告这些指针指向的内存区域不重叠。同时将 key->P 数组中本轮及下一轮要使用的子密钥提前 load 到局部 BF_LONG 标量变量中，用这些标量代替原 BF_ENC 宏中的 p[index] 间接访问，以减少指针追逐并增加指令级并行。] |
| 3 | try_flags | 1.129x  [flags: -mllvm --slp-max-reg-size=5] |
| 4 | try_pragma | 1.078x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 0.964x  [rewrite(utils/BF_encrypt): 将 BF_encrypt 函数内部根据 encrypt 标志分支的展开宏调用序列重写为一个单一的 for 循环：用循环索引 i 从 1 遍历到 16（若 BF_ROUNDS==20 则到 20），正向加密时调用 BF_ENC(r,l,s,p[i]) 与 BF_ENC(l,r,s,p[i+1]) 交替，反向解密时逆序调用；在循环外处理第一轮和最后一轮的 l^=p[...] 与 r^=p[...]。同时将 key->P 数组的前几轮元素在循环外一次性加载到局部 BF_LONG 数组中，减少循环内指针解引用。这样可以给编译器一个清晰的循环体，便于自动循环展开与向量化，并显著降低代码体积以提升 icache 利用率。] |
| 6 | try_flags | 1.366x  [flags: -mllvm -vector-combine-max-scan-instrs=64] |
| 7 | rewrite_source | 1.085x  [rewrite(utils/BF_encrypt): 在 BF_encrypt 函数定义前添加 __attribute__((always_inline))，强制编译器将其内联到调用者 BF_cfb64_encrypt 的循环中，从而将 16 轮 Feistel 运算嵌入循环体，为向量化和指令调度提供更大窗口。] |
| 8 | try_pragma | 0.499x  [pragma: #pragma clang loop vectorize(enable)] |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-scev-check-threshold=16 -mllvm -slp-max-vf=8 -mllvm -vectorize-num-stores-pred=2] |

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>0.9994x</b>（基线 1.15 ms，hash 校验，24/51 次为正）</summary>

- 任务 `c3_cb020`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1539x　最终确认：**0.9994x**　IQR [0.9926, 1.0118]　base_cv=4.4% best_cv=3.8%
- 最终采纳编译选项：`-mllvm -slp-max-look-ahead-depth=7`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.119x  [flags: -mllvm -slp-max-look-ahead-depth=3 -mllvm -vectorize-num-stores-pred=4] |
| 2 | rewrite_source | 1.081x  [rewrite(utils/BF_encrypt): 将 BF_encrypt 中手动展开的 16 轮加密/解密替换为一个 for 循环（i=1..16），根据 encrypt 标志计算 P 数组索引以统一方向；同时将 data 和 key 指针声明为 restrict 和 const，以移除可能的别名依赖。循环体内保留 BF_ENC 宏调用，依赖编译器完全展开和向量化。] |
| 3 | try_flags | 1.067x  [flags: -mllvm -slp-max-vf=0] |
| 4 | try_pragma | 1.020x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 0.926x  [rewrite(utils/BF_encrypt): Keep the manual 16-round unrolling. Add const and restrict qualifiers to the local pointer declarations: declare p and s as 'register const BF_LONG *restrict' to indicate key and S-box arrays do not alias *data. Also change the function signature to modern style with BF_LONG *restrict data, const BF_KEY *restrict key, to allow whole-function alias analysis. This |
| 6 | try_flags | 1.000x  [flags: -mllvm -slp-max-look-ahead-depth=3 -mllvm -vectorize-num-stores-pred=4] |
| 7 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpmch6ta7h/rw_utils_utils_shadow/polybench.c:992:4: error: use of undeclared identifier 'uint64_t' |
| 8 | try_flags | 1.154x  [flags: -mllvm -slp-max-look-ahead-depth=7] |
| 9 | try_pragma | 0.759x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>0.9978x</b>（基线 0.98 ms，hash 校验，21/51 次为正）</summary>

- 任务 `c3_cb014`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0387x　最终确认：**0.9978x**　IQR [0.9830, 1.0133]　base_cv=5.2% best_cv=4.8%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.029x  [rewrite: Replace the character-by-character hex parsing loop with a lookup-table (LUT) approach that processes two hex digits per iteration, directly storing a byte without conditional branches. Use a precomputed 256-byte table mapping '0'-'9','A'-'F' to 0-15 and others to an invalid sentinel, then convert two characters at a time, combining into a byte and storing in the key array, while |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 0.987x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 1.039x  [rewrite: Manually unroll the main hex‑parsing while‑loop by a factor of 4: process 8 hex characters (4 key bytes) per iteration, using the existing LUT to convert each pair. Accumulate these 4 bytes in local scalar variables during the unrolled iteration and store them to the key array only at the end of the unrolled block, reducing loop‑termination checks, pointer updates, and store traf |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 0.971x  [rewrite: Merge the four per‑byte validity checks in the unrolled hex‑parsing loop into a single bitwise‑OR of all nibble values and a single conditional branch; similarly merge the two nibble checks in the scalar tail loop and simplify the odd‑digit check to use the same combined validation pattern, reducing the total number of conditional branches in the hot path.] |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>1.0057x</b>（基线 1.54 ms，hash 校验，27/51 次为正）</summary>

- 任务 `c3_cb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3429x　最终确认：**1.0057x**　IQR [0.7834, 1.1373]　base_cv=22.1% best_cv=23.4%
- 最终采纳编译选项：`-mllvm -unroll-threshold=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.183x  [flags: -mllvm --pragma-unroll-threshold=8] |
| 2 | rewrite_source | 1.199x  [rewrite: Eliminate early exits in the hex-parsing while-loop. Replace the if-else chain that dispatches on character ranges with a static, compile-time-initialized 256-byte lookup table (valid hex digit -> 4-bit nibble, invalid -> a sentinel like -1). Inside the loop only perform `by = (by << 4) / hexval[(unsigned char)ch];` and accumulate an error flag via OR. After the loop, if error fl |
| 3 | try_flags | 1.000x  [flags: -mllvm --pragma-unroll-threshold=8] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp_5ffvt59/security_rijndael_encode_rewrite.c:259:23: note: previous initialization is here |
| 6 | try_flags | 1.343x  [flags: -mllvm -unroll-threshold=200] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 1.062x  [rewrite: Transform the key-parsing while loop to read two hex characters at a time, combine them into a single key byte using the lookup table, and store it directly. This breaks the cross-iteration `by` accumulator dependency, halves the iteration count, and provides the compiler with independent store operations that are easier to vectorize (SLP) and unroll. Keep the same error-detectio |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>0.9840x</b>（基线 1.02 ms，hash 校验，20/51 次为正）</summary>

- 任务 `c3_cb016`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1877x　最终确认：**0.9840x**　IQR [0.7366, 1.0145]　base_cv=29.1% best_cv=26.7%
- 最终采纳编译选项：`-mllvm -loop-distribute-scev-check-threshold=100`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.188x  [flags: -mllvm -loop-distribute-scev-check-threshold=100] |
| 2 | rewrite_source | 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp7bgupmky/rw_utils_utils_shadow/polybench.c:773:13: error: call to undeclared function 'sha1_transform'; ISO C99 and later do not support implicit function declarations [-Wimplicit-function-declaration] |
| 3 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 4 | try_flags | 1.080x  [flags: -mllvm -vector-combine-max-scan-instrs=8] |
| 5 | rewrite_source | 1.019x  [rewrite(utils/sha_stream): Inline sha_update’s body directly into sha_stream’s while loop. Replace the function call with a for‑loop that processes data[] in 64‑byte blocks, updating sha_info‑>state in place. Also provide the sha1_transform implementation in the same translation unit so that the inlined code compiles. This removes the only opaque call from the loop, exposing the inner blo |
| 6 | try_flags | 1.000x  [flags: -mllvm -loop-distribute-scev-check-threshold=100] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 0.999x  [rewrite(utils/sha_stream): Increase BLOCK_SIZE from 8192 to 65536 (8× larger) in sha_stream. This reduces the number of fread calls and while‑loop iterations, lowering I/O and loop‑control overhead. The sha_update call and its inner 64‑byte block loop remain identical, guaranteeing correctness. Use the original source version (base=original) and also apply the current best flags (-mllvm - |
| 9 | try_flags | 1.000x  [flags: -mllvm -loop-distribute-scev-check-threshold=100] |

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>1.0092x</b>（基线 18854.65 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c3_pb030`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0070x　最终确认：**1.0092x**　IQR [1.0086, 1.0133]　base_cv=0.0% best_cv=0.2%
- 最终采纳编译选项：`-mllvm -slp-threshold=-4 -mllvm -slp-schedule-budget=512 -mllvm -slp-max-stride=2`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.002x  [flags: -mllvm -slp-threshold=-4 -mllvm -slp-schedule-budget=512 -mllvm -slp-max-stride=2] |
| 2 | rewrite_source | 1.000x  [rewrite: Replace 2D array indexing in the innermost loop with pointer walking: for each i and t, set row pointers double *r_im1 = A[i-1], *r_i = A[i], *r_ip1 = A[i+1]; then in the j loop compute sum using r_im1[j-1+j] etc. and increment pointers after store to A[i][j] (or simply use pointer arithmetic with offsets to avoid incrementing). Aim to reduce address computation overhead and regi |
| 3 | try_flags | 1.001x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 0.777x  [rewrite: Apply spatial loop tiling to the i and j loops. Choose a tile size of 32×32 (fits in 1.3 MiB L1d). Traverse the tiles in wavefront order (i.e., for each diagonal where i+j is constant) to maintain the Gauss-Seidel dependency: all tiles that contain dependencies of a tile are guaranteed to have been computed earlier. Within each tile, retain the original inner-loop computation. Th |
| 6 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-int-reassociations=1 -mllvm -licm-max-num-fp-reassociations=16] |
| 7 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 8 | rewrite_source | 1.007x  [rewrite: 对最内层循环进行标量提升：在i次外层循环内，预先加载A[i-1]行的初始三个标量（对应j-1,j,j+1）及A[i+1]行的相同三个标量；然后在j循环中维护当前行A[i]的三个滑动标量（left,mid,right），每次迭代只需加载A[i][j+1]、A[i-1][j+1]、A[i+1][j+1]三个新值，其余通过寄存器滑动传递，将9次load减少为3次，大幅降低访存压力。] |
| 9 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |

</details>

<details><summary><b>symm</b> — 中位加速比 <b>6.6738x</b>（基线 4280.78 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb006`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：7.1437x　最终确认：**6.6738x**　IQR [5.3966, 12.3216]　base_cv=13.4% best_cv=44.0%
- 最终采纳编译选项：`-mllvm -licm-mssa-optimization-cap=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.017x  [flags: -mllvm -licm-mssa-optimization-cap=200] |
| 2 | rewrite_source | 2.267x  [rewrite: 拆分循环：将原 j 循环内的 k 循环体分成两个独立部分，先将 C[k][j] 更新部分重组为 for(k) for(j) 以得到连续内存访问（row-major），再保持 temp2 归约部分在原 j 循环内以确保数值一致，从而消除 LICM missed loads 和改善向量化机会。] |
| 3 | try_flags | 2.107x  [flags: -mllvm -licm-max-num-uses-traversed=24] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 7.144x  [rewrite: 在文件顶部为 C、A、B 添加 restrict 指针声明消除跨数组别名；在 i 循环内、j 循环外，将 A[i][0..i-1] 复制到局部栈数组 A_local（长度固定为 _PB_M），并在 temp2 归约的 k 循环中使用 A_local[k] 替代 A[i][k]，消除 load 别名，促进 LICM 外提和 LoopVectorize 向量化该归约循环；同时保持所有浮点运算顺序不变以确保数值一致性。] |
| 6 | try_flags | 5.424x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 7 | try_pragma | 5.555x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 6.164x  [rewrite: 对 j 循环进行分块（tile size 64），将每个 i 迭代内的 C[k][j] 更新、temp2 归约和 C[i][j] 更新融合在一个 j_tile 循环内：对于每个 tile，先初始化 temp2_vec 在该 tile 内的部分，然后遍历 k 更新 C[k][j_tile..j_tile+63] 并累加 temp2_vec，最后计算 C[i][j_tile..j_tile+63]。保持所有浮点运算顺序与原始一致，即 k 循环仍是 0..i-1，j 顺序不变。同时保留 restrict 和 A_local。] |
| 9 | try_flags | 6.628x  [flags: -mllvm -aggressive-instcombine-max-scan-instrs=256] |

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>4.2281x</b>（基线 2941.46 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb007`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：4.2352x　最终确认：**4.2281x**　IQR [4.1740, 4.2591]　base_cv=1.0% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=128 -mllvm -vectorize-scev-check-threshold=16`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.007x  [flags: -mllvm -vectorize-memory-check-threshold=32] |
| 2 | rewrite_source | 4.144x  [rewrite: Swap the loop order from for(k) for(j) to for(j) for(k), making the k loop the innermost. This changes A[j][k] and B[j][k] to unit-stride accesses, while preserving the per-(i,j) k-accumulation order.] |
| 3 | try_flags | 4.157x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 4 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 5.59e-03 at index 3840 (ref=1.78, opt=1.79), epsilon=1.00e-04 |
| 5 | rewrite_source | 4.155x  [rewrite: Merge the beta scaling loop into the accumulation loop: remove the separate j loop that does C[i][j] *= beta, and instead initialize cij = C[i][j] * beta inside the j loop before the k accumulation. This eliminates one load and one store per (i,j) pair, as cij is then accumulated and written back once.] |
| 6 | try_flags | 4.235x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=128 -mllvm -vectorize-scev-check-threshold=16] |
| 7 | try_pragma | 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 5.59e-03 at index 3840 (ref=1.78, opt=1.79), epsilon=1.00e-04 |
| 8 | rewrite_source | 1.009x  [rewrite: Tile the j and k loops for each i: split the j loop into blocks of size JB (e.g., 32) and the k loop into blocks of size KB (e.g., 256). For each i, iterate over j-blocks, then for each j-block iterate over k-blocks, and inside accumulate for each j in the block over k in the k-block using a local array of scalar accumulators (size JB) to hold partial sums of C[i][j]. After the k |
| 9 | try_flags | 4.220x  [flags: -mllvm -partial-unrolling-threshold=400] |

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>1.6738x</b>（基线 1100.93 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb008`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.8260x　最终确认：**1.6738x**　IQR [1.6638, 1.9439]　base_cv=9.0% best_cv=0.0%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=256 -mllvm --instcombine-max-num-phis=8 -mllvm --licm-mssa-max-acc-promotion=256`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.086x  [flags: -mllvm -vectorize-num-stores-pred=8] |
| 2 | rewrite_source | 1.756x  [rewrite: Swap the j and k loops in the update statement: move the j loop outside (j=0..i) and inside use a scalar accumulator tmp = C[i][j]; then an inner k loop (k=0..M-1) performing tmp += alpha * A[i][k] * A[j][k]; finally store tmp back to C[i][j]. Keep the beta scaling loop unchanged.] |
| 3 | try_flags | 1.818x  [flags: -mllvm --partial-unrolling-threshold=256 -mllvm --instcombine-max-num-phis=8 -mllvm --licm-mssa-max-acc-promotion=256] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 1.084x  [rewrite: Apply cache tiling on the i and j loops with a block size of 64: loop i in blocks of BI, loop j in blocks of BJ (BJ <= BI to respect triangular constraint), then perform the original scalar-accumulator k-loop inside the tile. This reuses A[i][k] and A[j][k] within the tile before moving to the next block, improving L1/L2 cache locality and reducing register spill pressure.] |
| 6 | try_flags | 1.803x  [flags: -mllvm -partial-unrolling-threshold=1024 -mllvm --slp-recursion-max-depth=64 -mllvm --pragma-unroll-and-jam-threshold=64] |
| 7 | rewrite_source | 1.826x  [rewrite: 对 k 循环进行分块：将原 for(k=0; k<_PB_M; k++) 改为外层 kk 循环（块大小 KK=256），内部保持原有 i、j 循环结构，最内层仍为 for(k=kk; k<min(kk+256,_PB_M); k++)。即循环顺序变为：for(kk=0; kk<_PB_M; kk+=256) → for(i=0; i<_PB_N; i++) → for(j=0; j<=i; j++) → for(k=kk; k<min(kk+256,_PB_M); k++)，且 j 循环内仍使用 tmp=0; for(k)... C[i][j]+=alpha*A[i][k]*A[j][k] 的标量累加器模式。此变换将 A 的列分成小块，使同一 k 块内的 A[i][k] 和 A[j][k] 在各级 cache 中充分复用，同时不破坏最内层 k 的连续访问 |
| 8 | try_flags | 1.783x  [flags: -mllvm -slp-max-reg-size=512] |
| 9 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>1.0000x</b>（基线 1.76 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c3_cb017`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0568x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 1.057x  [rewrite(utils/adpcm_coder): Transform adpcm_coder to load state->valprev and state->index into local variables at function entry, use them throughout the loop, and store them back at the end, removing aliasing between loads and stores. Additionally, manually unroll the main for-loop by a factor of 2 (processing two samples per iteration) to reduce branch overhead and improve instruction s |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_pragma | 0.948x  [pragma: #pragma clang loop vectorize(enable)] |
| 5 | rewrite_source | 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=6227febad457, opt=9295649d4621) |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_pragma | 1.035x  [pragma: #pragma clang loop vectorize(enable)] |
| 8 | rewrite_source | 1.021x  [rewrite(utils/adpcm_coder): 在 adpcm_coder 的 2x 展开主循环中，利用 bufferstep 每次迭代开始必定为 1 的特性，将前后两个样本的输出打包步骤硬编码：第一个样本直接计算 outputbuffer = (delta << 4) & 0xf0，第二个样本直接计算 *outp++ = (delta & 0x0f) / outputbuffer；移除 bufferstep 变量和 if 语句。尾部和剩余样本处理做相应调整但保持等价。] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>1.0036x</b>（基线 1.14 ms，hash 校验，29/51 次为正）</summary>

- 任务 `c3_cb018`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0399x　最终确认：**1.0036x**　IQR [0.9796, 1.0260]　base_cv=27.6% best_cv=29.0%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.512x  [rewrite(utils/adpcm_decoder): In adpcm_decoder, restructure the loop so that each iteration consumes a full input byte and produces two output shorts (two 4‑bit delta values). Extract both deltas directly from the byte without the bufferstep toggle, then compute valpred sequentially for the two samples. This eliminates the branch on bufferstep, halves loop back‑edge trips, and allows the  |
| 3 | try_pragma | 1.040x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | rewrite_source | 0.608x  [rewrite(utils/adpcm_decoder): In adpcm_decoder, unroll the main `for(; len>0; len--)` loop by a factor of 2. Keep the original bufferstep toggle logic and duplicate the whole body for a second sample inside the same iteration, updating valpred sequentially. After the loop, handle any remaining single sample. This halves the back-edge count and loop overhead without altering the branch‑pre |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 0.985x  [rewrite(utils/adpcm_decoder): 在 adpcm_decoder 的 for 循环中，用算术表达式 `vpdiff = (step * (2 * (delta & 7) + 1)) >> 3` 替换原有的 4 个 if 条件累加，消除 delta & 4/2/1 三个分支；将 sign 分支改为三元表达式 `valpred += (sign ? -vpdiff : vpdiff)`；将 clamp 条件改为嵌套三元表达式 `valpred = (valpred > 32767) ? 32767 : (valpred < -32768) ? -32768 : valpred`。保留 bufferstep toggle 逻辑不变。预期减少循环内部条件分支数量和关键路径指令数，提高流出带宽。] |
| 8 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>1.0018x</b>（基线 0.94 ms，hash 校验，26/51 次为正）</summary>

- 任务 `c3_cb019`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：2.1203x　最终确认：**1.0018x**　IQR [0.7631, 1.0439]　base_cv=38.7% best_cv=38.6%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpdayljg2l/telecom_crc32_rewrite.c:129:1: error: unknown type name 'We' |
| 3 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 4 | rewrite_source | 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpmnzf1_yq/telecom_crc32_rewrite.c:129:1: error: unknown type name '我们被要求提供一个优化的' |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | rewrite_source | 1.071x  [rewrite: 在 crc32file 函数内部：将逐字节 fgetc 读取改为 fread 以 16KB 块批量读取文件到局部缓冲区，然后在一个内层 for 循环中遍历缓冲区，每次迭代处理连续的 4 个字节——使用原有的 256 项 CRC-32 查找表、通过四次串行字节更新（即依次执行 crc = table[(crc ^ buf[i+k]) & 0xFF] ^ (crc >> 8)）的方式手动展开循环。处理完一个缓冲区后继续读取下一块，直到文件结束。所有 CRC 计算严格沿用原始多项式与合并逻辑，保证输出数值与原始程序完全一致。] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_pragma | 失败 [try_pragma] pragma_hints 为空 |
| 9 | rewrite_source | 2.120x  [rewrite: 在 crc32file 内循环中，将现有的 4 字节手动展开（每次处理 buf[i..i+3]）扩展为 8 字节展开：每次处理 buf[i..i+7]，对 8 个字节依次执行 UPDC32 宏更新 oldcrc32（共 8 次串行查表更新），步长改为 8；余下不足 8 字节的部分用单独的逐字节循环处理。所有计算逻辑、表格和合并方式不变，严格保证 CRC 输出与原程序一致。] |

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>1.0581x</b>（基线 11.72 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c3_pb021`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0668x　最终确认：**1.0581x**　IQR [0.9686, 1.0812]　base_cv=4.6% best_cv=2.0%
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 0.989x  [rewrite: Introduce a local scalar accumulator `tmp` for the reduction on x[i] inside the outer loop, eliminating repeated loads and stores to x[i] in the inner loop. This should allow LICM to hoist the load of L[i][i] and GVN to eliminate redundant loads of x[j], and reduce register pressure.] |
| 3 | try_pragma | 0.964x  [pragma: #pragma clang loop vectorize(enable)] |
| 4 | rewrite_source | 1.049x  [rewrite: Manually unroll the inner j-loop by 4, using a local scalar accumulator 'sum' for x[i] to avoid repeated loads/stores, and hoist L[i][i] to a local variable 'diag' before the inner loop to eliminate the LICM-missed load.] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | rewrite_source | 1.067x  [rewrite: Apply loop tiling to the outer i-loop (block size B=256). Split the inner j-loop into two parts: first accumulate over j from 0 to ii (the start of the current tile) using previously computed x values; then accumulate over j from ii to i-1 using the tile’s freshly computed x values. This improves cache locality for x[j] accesses within the tile.] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_pragma | 1.015x  [pragma: #pragma clang loop vectorize(enable)] |

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>17.1274x</b>（基线 2189.96 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c3_pb009`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：17.0787x　最终确认：**17.1274x**　IQR [16.9768, 17.1544]　base_cv=0.7% best_cv=0.3%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200`
- 最终采纳了源码重写
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | rewrite_source | 16.803x  [rewrite: 循环交换：将j循环移入最内层，使i和k循环在外层，让A和B数组的访问变为连续模式（A[k][i]变为A[i][k]等），并调整累加逻辑以适应新顺序，同时保持数值等价。] |
| 3 | try_flags | 17.079x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 4 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 5 | rewrite_source | 16.587x  [rewrite: 手动展开内层j循环因子2，使用标量累加器acc0和acc1分别累加B[i][j]和B[i][j+1]，在k循环内累加，k循环结束后统一写回B[i][j]和B[i][j+1]；处理尾数保持原始语义与数值结果一致。] |
| 6 | try_flags | 16.706x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 7 | try_pragma | 失败 [try_pragma] 未找到匹配的 for 循环前缀 |
| 8 | rewrite_source | 15.936x  [rewrite: 对i循环和j循环进行cache tiling：将外层的i循环分成块大小TI=64，将内层的j循环（含alpha缩放）分成块大小TJ=256。在块内仍然保持原有k循环和j循环顺序，确保j循环可向量化。此变换通过限制每次处理的i和j范围使B的活跃行保持在L2/L1 cache中，提升数据重用率。] |
| 9 | try_flags | 16.756x  [flags: -mllvm -slp-max-look-ahead-depth=3] |

</details>

### ④ params-only（每步强制 try_flags）（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>1.2375x</b>（基线 1192.42 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb010`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.2756x　最终确认：**1.2375x**　IQR [1.2344, 1.2449]　base_cv=0.2% best_cv=0.2%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=30`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.008x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 2 | try_flags | 1.060x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 3 | try_flags | 1.214x  [flags: -mllvm -partial-unrolling-threshold=50 -mllvm --pragma-vectorize-scev-check-threshold=8 -mllvm --vectorize-num-stores-pred=64] |
| 4 | try_flags | 1.062x  [flags: -mllvm -partial-unrolling-threshold=100 -mllvm -jump-threading-threshold=10 -mllvm -constraint-elimination-max-rows=512] |
| 5 | try_flags | 1.175x  [flags: -mllvm -partial-unrolling-threshold=50] |
| 6 | try_flags | 1.221x  [flags: -mllvm -partial-unrolling-threshold=40] |
| 7 | try_flags | 1.276x  [flags: -mllvm -partial-unrolling-threshold=30] |
| 8 | try_flags | 1.252x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm -aggressive-instcombine-max-scan-instrs=1600] |
| 9 | try_flags | 1.251x  [flags: -mllvm -partial-unrolling-threshold=32] |

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>1.0000x</b>（基线 3484.70 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb011`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>adi</b> — 中位加速比 <b>1.0714x</b>（基线 6587.48 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb025`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0856x　最终确认：**1.0714x**　IQR [1.0606, 1.0716]　base_cv=0.7% best_cv=0.1%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=32 -mllvm --instcombine-guard-widening-window=64 -mllvm --unroll-max-iteration-count-to-analyze=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.068x  [flags: -mllvm -partial-unrolling-threshold=500] |
| 2 | try_flags | 1.071x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --licm-versioning-max-depth-threshold=64 -mllvm --slp-max-root-look-ahead-depth=-1] |
| 3 | try_flags | 1.071x  [flags: -mllvm -partial-unrolling-threshold=32 -mllvm -licm-max-num-int-reassociations=8] |
| 4 | try_flags | 1.070x  [flags: -mllvm -partial-unrolling-threshold=32 -mllvm --instcombine-maxarray-size=64] |
| 5 | try_flags | 1.070x  [flags: -mllvm -partial-unrolling-threshold=128] |
| 6 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=32 -mllvm -licm-max-num-int-reassociations=8] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=32 -mllvm -licm-max-num-int-reassociations=8] |
| 8 | try_flags | 1.086x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --instcombine-guard-widening-window=64 -mllvm --unroll-max-iteration-count-to-analyze=64] |
| 9 | try_flags | 1.085x  [flags: -mllvm -partial-unrolling-threshold=32] |

</details>

<details><summary><b>atax</b> — 中位加速比 <b>1.0000x</b>（基线 15.42 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb012`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>1.0016x</b>（基线 14.73 ms，hash 校验，21/35 次为正，⚠ 正确性门无效）</summary>

- 任务 `c4_cb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0262x　最终确认：**1.0016x**　IQR [0.9909, 1.0098]　base_cv=2.3% best_cv=1.9%
- 最终采纳编译选项：`-mllvm --unroll-peel-max-count=16`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.026x  [flags: -mllvm --unroll-peel-max-count=16] |
| 4 | try_flags | 1.019x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 5 | try_flags | 1.025x  [flags: -mllvm --unroll-optsize-threshold=128] |
| 6 | try_flags | 1.009x  [flags: -mllvm -partial-unrolling-threshold=128] |
| 7 | try_flags | 1.018x  [flags: -mllvm -licm-max-num-int-reassociations=4] |
| 8 | try_flags | 1.015x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -licm-max-num-int-reassociations=8 -mllvm -licm-mssa-max-acc-promotion=8] |
| 9 | try_flags | 1.026x  [flags: -mllvm --pragma-unroll-full-max-iterations=16] |

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>1.0021x</b>（基线 4.75 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c4_cb002`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0396x　最终确认：**1.0021x**　IQR [0.9975, 1.0078]　base_cv=2.1% best_cv=2.1%
- 最终采纳编译选项：`-mllvm -vectorize-num-stores-pred=4`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.040x  [flags: -mllvm -vectorize-num-stores-pred=4] |
| 2 | try_flags | 1.031x  [flags: -mllvm -slp-max-root-look-ahead-depth=4 -mllvm -vectorize-memory-check-threshold=8 -mllvm -slp-max-vf=4] |
| 3 | try_flags | 1.030x  [flags: -mllvm -slp-max-stride=2] |
| 4 | try_flags | 1.000x  [flags: -mllvm -vectorize-num-stores-pred=4] |
| 5 | try_flags | 1.000x  [flags: -mllvm -vectorize-num-stores-pred=16] |
| 6 | try_flags | 1.032x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 7 | try_flags | 1.038x  [flags: -mllvm --instcombine-max-num-phis=16] |
| 8 | try_flags | 1.016x  [flags: -mllvm --instcombine-negator-max-depth=32] |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-num-stores-pred=4] |

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>0.9926x</b>（基线 8.60 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb003`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0488x　最终确认：**0.9926x**　IQR [0.9812, 1.0102]　base_cv=2.0% best_cv=2.6%
- 最终采纳编译选项：`-mllvm --slp-max-root-look-ahead-depth=10`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.026x  [flags: -mllvm --instcombine-max-num-phis=8] |
| 2 | try_flags | 1.024x  [flags: -mllvm -loop-load-elimination-scev-check-threshold=16] |
| 3 | try_flags | 1.014x  [flags: -mllvm -slp-max-vf=8] |
| 4 | try_flags | 1.000x  [flags: -mllvm --instcombine-max-num-phis=8] |
| 5 | try_flags | 1.012x  [flags: -mllvm -slp-max-reg-size=256] |
| 6 | try_flags | 1.029x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 8 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 9 | try_flags | 1.049x  [flags: -mllvm --slp-max-root-look-ahead-depth=10] |

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.0065x</b>（基线 61.79 ms，hash 校验，9/9 次为正）</summary>

- 任务 `c4_cb004`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0100x　最终确认：**1.0065x**　IQR [1.0056, 1.0079]　base_cv=0.1% best_cv=0.3%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=400`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.010x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 3 | try_flags | 1.007x  [flags: -mllvm -partial-unrolling-threshold=500] |
| 4 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 5 | try_flags | 1.009x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm -slp-max-reg-size=128] |
| 6 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 8 | try_flags | 1.010x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 9 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=16] |

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>1.0000x</b>（基线 23.89 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb013`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>1.0472x</b>（基线 82.23 ms，hash 校验，6/9 次为正）</summary>

- 任务 `c4_cb005`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3628x　最终确认：**1.0472x**　IQR [0.9471, 1.0775]　base_cv=86.1% best_cv=94.5%
- 最终采纳编译选项：`-mllvm -licm-mssa-max-acc-promotion=32`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.019x  [flags: -mllvm -licm-mssa-max-acc-promotion=16] |
| 2 | try_flags | 1.000x  [flags: -mllvm -licm-mssa-max-acc-promotion=16] |
| 3 | try_flags | 1.363x  [flags: -mllvm -licm-mssa-max-acc-promotion=32] |
| 4 | try_flags | 1.000x  [flags: -mllvm -licm-mssa-max-acc-promotion=32] |
| 5 | try_flags | 1.131x  [flags: -mllvm --unroll-threshold-default=8] |
| 6 | try_flags | 1.193x  [flags: -mllvm --slp-recursion-max-depth=128] |
| 7 | try_flags | 1.005x  [flags: -mllvm -jump-threading-phi-threshold=2] |
| 8 | try_flags | 1.250x  [flags: -mllvm -loop-load-elimination-scev-check-threshold=16] |
| 9 | try_flags | 1.040x  [flags: -mllvm --instcombine-simplify-vector-elts-depth=64] |

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>1.0137x</b>（基线 86.09 ms，hash 校验，7/7 次为正）</summary>

- 任务 `c4_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0128x　最终确认：**1.0137x**　IQR [1.0074, 1.0153]　base_cv=0.4% best_cv=0.2%
- 最终采纳编译选项：`-mllvm --slp-max-reg-size=0`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.002x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 3 | try_flags | 1.004x  [flags: -mllvm -vectorize-scev-check-threshold=32] |
| 4 | try_flags | 1.012x  [flags: -mllvm --slp-max-reg-size=10] |
| 5 | try_flags | 1.013x  [flags: -mllvm --slp-max-reg-size=0] |
| 6 | try_flags | 1.011x  [flags: -mllvm --partial-unrolling-threshold=256] |
| 7 | try_flags | 1.008x  [flags: -mllvm -partial-unrolling-threshold=256] |
| 8 | try_flags | 1.013x  [flags: -mllvm -partial-unrolling-threshold=512] |
| 9 | try_flags | 1.011x  [flags: -mllvm -slp-max-reg-size=0] |

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0498x</b>（基线 27155.22 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb016`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0636x　最终确认：**1.0498x**　IQR [1.0462, 1.0546]　base_cv=1.0% best_cv=0.8%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200 -mllvm -unroll-threshold=300`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.060x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 2 | try_flags | 1.019x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 3 | try_flags | 1.059x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -licm-max-num-uses-traversed=16] |
| 4 | try_flags | 1.057x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 5 | try_flags | 1.057x  [flags: -mllvm -partial-unrolling-threshold=250 -mllvm -licm-max-num-uses-traversed=24] |
| 6 | try_flags | 1.019x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 8 | try_flags | 1.064x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -unroll-threshold=300] |
| 9 | try_flags | 1.033x  [flags: -mllvm -partial-unrolling-threshold=150] |

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>1.0043x</b>（基线 3.12 ms，hash 校验，27/51 次为正）</summary>

- 任务 `c4_cb007`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.6085x　最终确认：**1.0043x**　IQR [0.8550, 1.1266]　base_cv=17.4% best_cv=17.0%
- 最终采纳编译选项：`-mllvm -slp-max-look-ahead-depth=3`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.514x  [flags: -mllvm -slp-max-vf=32 -mllvm -licm-mssa-max-acc-promotion=16] |
| 2 | try_flags | 1.479x  [flags: -mllvm --licm-versioning-max-depth-threshold=128] |
| 3 | try_flags | 1.393x  [flags: -mllvm -licm-mssa-max-acc-promotion=32] |
| 4 | try_flags | 1.390x  [flags: -mllvm -licm-max-num-fp-reassociations=256] |
| 5 | try_flags | 1.124x  [flags: -mllvm -gvn-max-block-speculations=64 -mllvm -slp-max-look-ahead-depth=2] |
| 6 | try_flags | 1.222x  [flags: -mllvm --unroll-peel-max-count=64] |
| 7 | try_flags | 1.453x  [flags: -mllvm -vectorize-memory-check-threshold=128] |
| 8 | try_flags | 1.110x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 9 | try_flags | 1.609x  [flags: -mllvm -slp-max-look-ahead-depth=3] |

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>0.9972x</b>（基线 3.27 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb008`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.6256x　最终确认：**0.9972x**　IQR [0.9696, 1.0524]　base_cv=27.1% best_cv=27.5%
- 最终采纳编译选项：`-mllvm -licm-mssa-optimization-cap=2000 -mllvm -licm-max-num-uses-traversed=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.529x  [flags: -mllvm -licm-versioning-max-depth-threshold=8] |
| 2 | try_flags | 1.626x  [flags: -mllvm -licm-mssa-optimization-cap=2000 -mllvm -licm-max-num-uses-traversed=64] |
| 3 | try_flags | 1.470x  [flags: -mllvm -licm-mssa-optimization-cap=4000 -mllvm -licm-mssa-max-acc-promotion=200] |
| 4 | try_flags | 1.500x  [flags: -mllvm -licm-max-num-uses-traversed=256] |
| 5 | try_flags | 1.566x  [flags: -mllvm --instcombine-maxarray-size=16] |
| 6 | try_flags | 1.493x  [flags: -mllvm -aggressive-instcombine-max-scan-instrs=512] |
| 7 | try_flags | 1.215x  [flags: -mllvm -gvn-hoist-max-depth=200] |
| 8 | try_flags | 1.193x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 9 | try_flags | 1.023x  [flags: -mllvm -partial-unrolling-threshold=60] |

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>1.0125x</b>（基线 1.99 ms，hash 校验，27/51 次为正，⚠ 正确性门无效）</summary>

- 任务 `c4_cb009`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.7706x　最终确认：**1.0125x**　IQR [0.9378, 1.0674]　base_cv=11.3% best_cv=8.4%
- 最终采纳编译选项：`-mllvm -licm-mssa-max-acc-promotion=300`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.348x  [flags: -mllvm -licm-max-num-uses-traversed=24] |
| 2 | try_flags | 1.471x  [flags: -mllvm -licm-mssa-max-acc-promotion=200] |
| 3 | try_flags | 1.594x  [flags: -mllvm -licm-mssa-optimization-cap=200 -mllvm -slp-max-vf=8 -mllvm -licm-max-num-uses-traversed=24] |
| 4 | try_flags | 1.396x  [flags: -mllvm -licm-max-num-uses-traversed=48] |
| 5 | try_flags | 1.771x  [flags: -mllvm -licm-mssa-max-acc-promotion=300] |
| 6 | try_flags | 1.592x  [flags: -mllvm -slp-max-vf=16] |
| 7 | try_flags | 1.342x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 8 | try_flags | 1.571x  [flags: -mllvm -licm-max-num-uses-traversed=24] |
| 9 | try_flags | 1.726x  [flags: -mllvm --openmp-ir-builder-unroll-threshold-factor=2] |

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>1.0341x</b>（基线 23.46 ms，hash 校验，16/21 次为正）</summary>

- 任务 `c4_cb010`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：6.8874x　最终确认：**1.0341x**　IQR [1.0157, 1.0866]　base_cv=76.5% best_cv=75.1%
- 最终采纳编译选项：`-mllvm --instcombine-simplify-vector-elts-depth=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 6.542x  [flags: -mllvm -vectorize-memory-check-threshold=256 -mllvm -slp-max-vf=8] |
| 2 | try_flags | 5.654x  [flags: -mllvm -jump-threading-threshold=256] |
| 3 | try_flags | 6.887x  [flags: -mllvm --instcombine-simplify-vector-elts-depth=32] |
| 4 | try_flags | 5.125x  [flags: -mllvm -slp-max-vf=32] |
| 5 | try_flags | 6.597x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -gvn-hoist-max-bbs=8 -mllvm -slp-max-vf=16] |
| 6 | try_flags | 1.022x  [flags: -mllvm -aggressive-instcombine-max-scan-instrs=1000] |
| 7 | try_flags | 4.807x  [flags: -mllvm --unroll-max-upperbound=16] |
| 8 | try_flags | 5.472x  [flags: -mllvm -earlycse-mssa-optimization-cap=128] |
| 9 | try_flags | 5.666x  [flags: -mllvm --pragma-unroll-and-jam-threshold=128 -mllvm --unroll-max-percent-threshold-boost=16] |

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>1.0000x</b>（基线 4784.70 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c4_pb001`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善]content empty, falling back to reasoning_content on attempt 1 |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>1.0000x</b>（基线 4806.28 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb002`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>1.0107x</b>（基线 132.08 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb022`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.0129x　最终确认：**1.0107x**　IQR [0.9944, 1.0201]　base_cv=1.3% best_cv=1.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=300`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.005x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 4 | try_flags | 1.013x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 5 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 6 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 8 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 9 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=300] |

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>1.0530x</b>（基线 240.89 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb014`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0460x　最终确认：**1.0530x**　IQR [1.0436, 1.0542]　base_cv=0.3% best_cv=0.6%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.021x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -openmp-ir-builder-unroll-threshold-factor=1.5] |
| 2 | try_flags | 1.022x  [flags: -mllvm -partial-unrolling-threshold=250 -mllvm -licm-max-num-uses-traversed=32] |
| 3 | try_flags | 1.036x  [flags: -mllvm -unroll-threshold-aggressive=1500] |
| 4 | try_flags | 1.046x  [flags: -mllvm --partial-unrolling-threshold=64] |
| 5 | try_flags | 1.046x  [flags: -mllvm --partial-unrolling-threshold=64] |
| 6 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=64] |
| 7 | try_flags | 1.041x  [flags: -mllvm -partial-unrolling-threshold=64] |
| 8 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=64] |
| 9 | try_flags | 1.046x  [flags: -mllvm -partial-unrolling-threshold=64] |

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.0537x</b>（基线 2.18 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb017`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.0498x　最终确认：**1.0537x**　IQR [0.9710, 1.1381]　base_cv=2.6% best_cv=6.4%
- 最终采纳编译选项：`-mllvm -vectorize-memory-check-threshold=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.023x  [flags: -mllvm -licm-max-num-uses-traversed=8] |
| 2 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=8] |
| 3 | try_flags | 1.000x  [flags: -mllvm -slp-max-look-ahead-depth=8] |
| 4 | try_flags | 1.050x  [flags: -mllvm -vectorize-memory-check-threshold=32] |
| 5 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=32] |
| 6 | try_flags | 1.023x  [flags: -mllvm --licm-mssa-max-acc-promotion=32] |
| 7 | try_flags | 1.002x  [flags: -mllvm -vectorize-memory-check-threshold=128] |
| 8 | try_flags | 1.002x  [flags: -mllvm --slp-max-stride=64] |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=32] |

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>0.9816x</b>（基线 975.32 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c4_pb026`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0147x　最终确认：**0.9816x**　IQR [0.9694, 1.0208]　base_cv=1.6% best_cv=1.1%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.015x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 3 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 4 | try_flags | 1.011x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 5 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 6 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 7 | try_flags | 1.001x  [flags: -mllvm -slp-max-reg-size=256] |
| 8 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 9 | try_flags | 1.009x  [flags: -mllvm -slp-max-look-ahead-depth=5] |

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>1.0000x</b>（基线 33607.70 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c4_pb023`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>1.0070x</b>（基线 195.90 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb003`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.1098x　最终确认：**1.0070x**　IQR [0.9572, 1.0680]　base_cv=2.5% best_cv=3.3%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=256`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.062x  [flags: -mllvm -vectorize-scev-check-threshold=16] |
| 2 | try_flags | 1.080x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 3 | try_flags | 1.081x  [flags: -mllvm --slp-max-look-ahead-depth=-1] |
| 4 | try_flags | 1.089x  [flags: -mllvm -licm-max-num-uses-traversed=128] |
| 5 | try_flags | 1.105x  [flags: -mllvm -vectorize-num-stores-pred=4] |
| 6 | try_flags | 1.110x  [flags: -mllvm --partial-unrolling-threshold=256] |
| 7 | try_flags | 1.073x  [flags: -mllvm -licm-max-num-uses-traversed=512] |
| 8 | try_flags | 1.077x  [flags: -mllvm -partial-unrolling-threshold=256 -mllvm -vectorize-scev-check-threshold=16 -mllvm -vectorize-num-stores-pred=8] |
| 9 | try_flags | 1.075x  [flags: -mllvm -vectorize-scev-check-threshold=32] |

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.0125x</b>（基线 21.43 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb004`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0320x　最终确认：**1.0125x**　IQR [0.8981, 1.0373]　base_cv=3.9% best_cv=4.8%
- 最终采纳编译选项：`-mllvm -vectorize-scev-check-threshold=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.011x  [flags: -mllvm -slp-max-look-ahead-depth=8] |
| 5 | try_flags | 1.032x  [flags: -mllvm -vectorize-scev-check-threshold=32] |
| 6 | try_flags | 1.000x  [flags: -mllvm -vectorize-scev-check-threshold=32] |
| 7 | try_flags | 1.000x  [flags: -mllvm -vectorize-scev-check-threshold=32] |
| 8 | try_flags | 1.007x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-scev-check-threshold=32] |

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>0.9933x</b>（基线 23.29 ms，numeric 校验，0/3 次为正）</summary>

- 任务 `c4_pb005`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0262x　最终确认：**0.9933x**　IQR [0.9889, 0.9963]　base_cv=0.1% best_cv=0.4%
- 最终采纳编译选项：`-mllvm --slp-recursion-max-depth=8`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.009x  [flags: -mllvm -vectorize-memory-check-threshold=4] |
| 2 | try_flags | 1.010x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 3 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 4 | try_flags | 1.009x  [flags: -mllvm --licm-max-num-int-reassociations=8] |
| 5 | try_flags | 1.019x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 6 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 7 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 8 | try_flags | 1.001x  [flags: -mllvm -licm-max-num-uses-traversed=12] |
| 9 | try_flags | 1.026x  [flags: -mllvm --slp-recursion-max-depth=8] |

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>1.0000x</b>（基线 7172.84 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb018`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.0623x</b>（基线 1452.27 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c4_pb027`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.0660x　最终确认：**1.0623x**　IQR [1.0616, 1.0700]　base_cv=0.0% best_cv=0.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=4000 -mllvm -slp-max-vf=4`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.066x  [flags: -mllvm -partial-unrolling-threshold=2400 -mllvm -unroll-threshold-aggressive=2400] |
| 2 | try_flags | 1.066x  [flags: -mllvm -partial-unrolling-threshold=4000 -mllvm -slp-max-vf=4] |
| 3 | try_flags | 1.063x  [flags: -mllvm -partial-unrolling-threshold=4000 -mllvm -unroll-threshold=2000 -mllvm -slp-threshold=0] |
| 4 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=4000 -mllvm -slp-max-vf=4] |
| 5 | try_flags | 1.065x  [flags: -mllvm -partial-unrolling-threshold=4000] |
| 6 | try_flags | 1.000x  [flags: -mllvm -slp-max-vf=0] |
| 7 | try_flags | 1.001x  [flags: -mllvm --instcombine-simplify-vector-elts-depth=16] |
| 8 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=4000 -mllvm -slp-max-vf=4] |
| 9 | try_flags | 1.001x  [flags: -mllvm -slp-max-vf=16] |

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>1.0470x</b>（基线 1.59 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb028`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0818x　最终确认：**1.0470x**　IQR [1.0272, 1.7305]　base_cv=33.2% best_cv=1.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=800`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.082x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 3 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 4 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 5 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 6 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 7 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 8 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |
| 9 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=800] |

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>1.1735x</b>（基线 1181.78 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb029`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.1811x　最终确认：**1.1735x**　IQR [1.1642, 1.1786]　base_cv=0.5% best_cv=0.1%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=256`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.174x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 2 | try_flags | 1.177x  [flags: -mllvm -partial-unrolling-threshold=300 -mllvm -slp-max-vf=4 -mllvm -slp-max-reg-size=256] |
| 3 | try_flags | 1.151x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm -slp-max-reg-size=512 -mllvm -slp-max-vf=8] |
| 4 | try_flags | 1.166x  [flags: -mllvm -partial-unrolling-threshold=250] |
| 5 | try_flags | 1.181x  [flags: -mllvm --partial-unrolling-threshold=256] |
| 6 | try_flags | 1.167x  [flags: -mllvm --partial-unrolling-threshold=256 -mllvm --slp-max-root-look-ahead-depth=-1] |
| 7 | try_flags | 1.167x  [flags: -mllvm -partial-unrolling-threshold=256 -mllvm -slp-max-root-look-ahead-depth=0] |
| 8 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=256] |
| 9 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=256] |

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.1028x</b>（基线 7651.82 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb020`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.1078x　最终确认：**1.1028x**　IQR [1.1018, 1.1039]　base_cv=0.1% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=8000`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.104x  [flags: -mllvm -partial-unrolling-threshold=2000] |
| 4 | try_flags | 1.105x  [flags: -mllvm -partial-unrolling-threshold=3000] |
| 5 | try_flags | 1.106x  [flags: -mllvm -partial-unrolling-threshold=5000 -mllvm -licm-max-num-uses-traversed=16 -mllvm -pragma-vectorize-scev-check-threshold=64] |
| 6 | try_flags | 1.104x  [flags: -mllvm -partial-unrolling-threshold=7500] |
| 7 | try_flags | 1.003x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 8 | try_flags | 1.107x  [flags: -mllvm -partial-unrolling-threshold=8000] |
| 9 | try_flags | 1.108x  [flags: -mllvm -partial-unrolling-threshold=8000] |

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.0000x</b>（基线 6877.13 ms，numeric 校验，0/0 次为正）</summary>

- 任务 `c4_pb019`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>1.0311x</b>（基线 20.73 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb015`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.0723x　最终确认：**1.0311x**　IQR [0.9885, 1.0663]　base_cv=1.0% best_cv=3.9%
- 最终采纳编译选项：`-mllvm -vectorize-scev-check-threshold=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.017x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 2 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 3 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 4 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 5 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 6 | try_flags | 1.013x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 7 | try_flags | 1.072x  [flags: -mllvm -vectorize-scev-check-threshold=32] |
| 8 | try_flags | 1.030x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 9 | try_flags | 1.056x  [flags: -mllvm --instcombine-maxarray-size=16] |

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>0.9905x</b>（基线 1.12 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb011`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1147x　最终确认：**0.9905x**　IQR [0.9053, 1.1849]　base_cv=30.0% best_cv=30.1%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-vf=4 -mllvm -vectorize-memory-check-threshold=128`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.115x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-vf=4 -mllvm -vectorize-memory-check-threshold=128] |
| 6 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-vf=4 -mllvm -vectorize-memory-check-threshold=128] |
| 7 | try_flags | 1.020x  [flags: -mllvm -slp-max-vf=16] |
| 8 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-vf=4 -mllvm -vectorize-memory-check-threshold=128] |
| 9 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=64 -mllvm -slp-max-vf=4 -mllvm -vectorize-memory-check-threshold=128] |

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>0.9992x</b>（基线 1.89 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb012`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.9881x　最终确认：**0.9992x**　IQR [0.9805, 1.0173]　base_cv=6.0% best_cv=5.4%
- 最终采纳编译选项：`-mllvm --licm-max-num-int-reassociations=32`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.988x  [flags: -mllvm --licm-max-num-int-reassociations=32] |
| 2 | try_flags | 1.702x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 3 | try_flags | 1.839x  [flags: -mllvm -licm-mssa-optimization-cap=100] |
| 4 | try_flags | 1.000x  [flags: -mllvm --licm-max-num-int-reassociations=32] |
| 5 | try_flags | 1.000x  [flags: -mllvm --licm-max-num-int-reassociations=32] |
| 6 | try_flags | 1.649x  [flags: -mllvm -licm-mssa-max-acc-promotion=2] |
| 7 | try_flags | 1.644x  [flags: -mllvm -earlycse-mssa-optimization-cap=200 -mllvm -licm-max-num-int-reassociations=64] |
| 8 | try_flags | 1.688x  [flags: -mllvm -float2int-max-integer-bw=64 -mllvm -gvn-hoist-max-depth=50 -mllvm -gvn-max-hoisted=20] |
| 9 | try_flags | 1.010x  [flags: -mllvm -licm-max-num-fp-reassociations=8] |

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>1.0181x</b>（基线 3727.29 ms，hash 校验，3/3 次为正）</summary>

- 任务 `c4_pb024`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0255x　最终确认：**1.0181x**　IQR [1.0142, 1.0222]　base_cv=0.1% best_cv=0.5%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=95`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.025x  [flags: -mllvm -partial-unrolling-threshold=100 -mllvm -licm-versioning-invariant-threshold=0.1] |
| 2 | try_flags | 1.020x  [flags: -mllvm -partial-unrolling-threshold=200] |
| 3 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=8] |
| 4 | try_flags | 1.016x  [flags: -mllvm -partial-unrolling-threshold=125] |
| 5 | try_flags | 1.006x  [flags: -mllvm -vectorize-memory-check-threshold=128 -mllvm -licm-mssa-max-acc-promotion=200] |
| 6 | try_flags | 1.003x  [flags: -mllvm -gvn-hoist-max-chain-length=20] |
| 7 | try_flags | 1.022x  [flags: -mllvm -partial-unrolling-threshold=105] |
| 8 | try_flags | 1.000x  [flags: -mllvm --licm-mssa-max-acc-promotion=128] |
| 9 | try_flags | 1.025x  [flags: -mllvm -partial-unrolling-threshold=95] |

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>0.9998x</b>（基线 2.64 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb013`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.9356x　最终确认：**0.9998x**　IQR [0.9107, 1.0482]　base_cv=15.1% best_cv=15.8%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.261x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 2 | try_flags | 1.084x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 3 | try_flags | 1.217x  [flags: -mllvm -licm-max-num-uses-traversed=24] |
| 4 | try_flags | 1.605x  [flags: -mllvm --slp-min-reg-size=-2] |
| 5 | try_flags | 1.754x  [flags: -mllvm -simple-loop-unswitch-memoryssa-threshold=128] |
| 6 | try_flags | 1.456x  [flags: -mllvm -licm-mssa-optimization-cap=100] |
| 7 | try_flags | 1.307x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 8 | try_flags | 1.637x  [flags: -mllvm --instcombine-maxarray-size=256] |
| 9 | try_flags | 1.936x  [flags: -mllvm -licm-max-num-uses-traversed=32] |

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>1.0118x</b>（基线 2.12 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c4_cb021`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.7967x　最终确认：**1.0118x**　IQR [0.9652, 1.0857]　base_cv=10.7% best_cv=10.4%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=200 -mllvm -slp-max-vf=16`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.278x  [flags: -mllvm --unroll-optsize-threshold=64] |
| 3 | try_flags | 1.660x  [flags: -mllvm --vectorize-scev-check-threshold=40] |
| 4 | try_flags | 1.033x  [flags: -mllvm -unroll-optsize-threshold=128] |
| 5 | try_flags | 1.771x  [flags: -mllvm -slp-max-vf=4] |
| 6 | try_flags | 1.484x  [flags: -mllvm -vectorize-scev-check-threshold=48] |
| 7 | try_flags | 1.000x  [flags: -mllvm -slp-max-vf=4] |
| 8 | try_flags | 1.637x  [flags: -mllvm -vectorize-scev-check-threshold=96] |
| 9 | try_flags | 1.797x  [flags: -mllvm -partial-unrolling-threshold=200 -mllvm -slp-max-vf=16] |

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>0.9982x</b>（基线 1.14 ms，hash 校验，22/51 次为正）</summary>

- 任务 `c4_cb020`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1751x　最终确认：**0.9982x**　IQR [0.9874, 1.0068]　base_cv=12.3% best_cv=13.6%
- 最终采纳编译选项：`-mllvm -vectorize-memory-check-threshold=8`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.175x  [flags: -mllvm -vectorize-memory-check-threshold=8]content empty, falling back to reasoning_content on attempt 1 |
| 4 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |
| 5 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |
| 6 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |
| 7 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |
| 8 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |
| 9 | try_flags | 1.000x  [flags: -mllvm -vectorize-memory-check-threshold=8] |

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>1.0046x</b>（基线 2.21 ms，hash 校验，28/51 次为正）</summary>

- 任务 `c4_cb014`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.9490x　最终确认：**1.0046x**　IQR [0.9793, 1.0157]　base_cv=11.2% best_cv=11.3%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=512 -mllvm -partial-unrolling-threshold=64 -mllvm -slp-max-reg-size=1024`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.125x  [flags: -mllvm -slp-max-vf=32] |
| 2 | try_flags | 1.689x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 3 | try_flags | 1.858x  [flags: -mllvm -slp-max-reg-size=512 -mllvm -licm-max-num-uses-traversed=128] |
| 4 | try_flags | 1.626x  [flags: -mllvm --unroll-threshold-default=64] |
| 5 | try_flags | 1.065x  [flags: -mllvm -slp-max-root-look-ahead-depth=4] |
| 6 | try_flags | 1.101x  [flags: -mllvm -licm-mssa-optimization-cap=512] |
| 7 | try_flags | 1.000x  [flags: -mllvm -slp-max-reg-size=512 -mllvm -licm-max-num-uses-traversed=128] |
| 8 | try_flags | 1.096x  [flags: -mllvm -partial-unrolling-threshold=256] |
| 9 | try_flags | 1.949x  [flags: -mllvm -licm-max-num-uses-traversed=512 -mllvm -partial-unrolling-threshold=64 -mllvm -slp-max-reg-size=1024] |

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>1.0343x</b>（基线 1.94 ms，hash 校验，27/51 次为正）</summary>

- 任务 `c4_cb015`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3952x　最终确认：**1.0343x**　IQR [0.8888, 1.1100]　base_cv=33.3% best_cv=30.9%
- 最终采纳编译选项：`-mllvm -slp-max-look-ahead-depth=6`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.245x  [flags: -mllvm -slp-max-reg-size=256 -mllvm -slp-max-vf=8]content empty, falling back to reasoning_content on attempt 1 |
| 5 | try_flags | 1.000x  [flags: -mllvm -slp-max-reg-size=256 -mllvm -slp-max-vf=8] |
| 6 | try_flags | 1.037x  [flags: -mllvm -slp-max-look-ahead-depth=3] |
| 7 | try_flags | 1.000x  [flags: -mllvm -slp-max-reg-size=256 -mllvm -slp-max-vf=8] |
| 8 | try_flags | 1.395x  [flags: -mllvm -slp-max-look-ahead-depth=6] |
| 9 | try_flags | 1.000x  [flags: -mllvm -slp-max-look-ahead-depth=6] |

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>0.9946x</b>（基线 1.25 ms，hash 校验，25/51 次为正）</summary>

- 任务 `c4_cb016`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3837x　最终确认：**0.9946x**　IQR [0.9765, 1.0427]　base_cv=10.1% best_cv=10.5%
- 最终采纳编译选项：`-mllvm --vectorize-scev-check-threshold=8`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.384x  [flags: -mllvm --vectorize-scev-check-threshold=8] |
| 2 | try_flags | 1.000x  [flags: -mllvm --vectorize-scev-check-threshold=8] |
| 3 | try_flags | 1.000x  [flags: -mllvm --vectorize-scev-check-threshold=8] |
| 4 | try_flags | 1.000x  [flags: -mllvm --vectorize-scev-check-threshold=8] |
| 5 | try_flags | 1.193x  [flags: -mllvm --instcombine-maxarray-size=32] |
| 6 | try_flags | 1.111x  [flags: -mllvm -unswitch-threshold=50 -mllvm -constraint-elimination-max-rows=1024] |
| 7 | try_flags | 1.203x  [flags: -mllvm -float2int-max-integer-bw=32] |
| 8 | try_flags | 1.214x  [flags: -mllvm -float2int-max-integer-bw=64] |
| 9 | try_flags | 1.158x  [flags: -mllvm -partial-unrolling-threshold=25 -mllvm -constraint-elimination-max-rows=2048 -mllvm -vectorize-memory-check-threshold=256] |

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>1.0015x</b>（基线 75534.74 ms，numeric 校验，3/3 次为正，⚠ 正确性门无效）</summary>

- 任务 `c4_pb030`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：4.0110x　最终确认：**1.0015x**　IQR [1.0014, 1.0015]　base_cv=0.0% best_cv=0.0%
- 最终采纳编译选项：`-mllvm -slp-threshold=-4`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 2.400x  [flags: -mllvm -licm-mssa-optimization-cap=8 -mllvm -licm-max-num-uses-traversed=16] |
| 2 | try_flags | 1.332x  [flags: -mllvm -earlycse-mssa-optimization-cap=8] |
| 3 | try_flags | 1.438x  [flags: -mllvm -partial-unrolling-threshold=50 -mllvm -slp-threshold=-1] |
| 4 | try_flags | 1.043x  [flags: -mllvm -licm-max-num-fp-reassociations=4] |
| 5 | try_flags | 1.332x  [flags: -mllvm -licm-max-num-uses-traversed=128] |
| 6 | try_flags | 2.918x  [flags: -mllvm -licm-mssa-max-acc-promotion=8] |
| 7 | try_flags | 4.006x  [flags: -mllvm -licm-max-num-uses-traversed=4 -mllvm -licm-mssa-optimization-cap=8] |
| 8 | try_flags | 4.006x  [flags: -mllvm -gvn-hoist-max-depth=-1 -mllvm -gvn-hoist-max-chain-length=-1] |
| 9 | try_flags | 4.011x  [flags: -mllvm -slp-threshold=-4] |

</details>

<details><summary><b>symm</b> — 中位加速比 <b>1.0078x</b>（基线 898.86 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb006`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：1.0189x　最终确认：**1.0078x**　IQR [1.0071, 1.0086]　base_cv=0.1% best_cv=0.0%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=32 -mllvm --loop-idiom-vectorize-bytecmp-vf=8 -mllvm --slp-schedule-budget=10`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.019x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --loop-idiom-vectorize-bytecmp-vf=8 -mllvm --slp-schedule-budget=10] |
| 2 | try_flags | 1.008x  [flags: -mllvm --partial-unrolling-threshold=32] |
| 3 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --loop-idiom-vectorize-bytecmp-vf=8 -mllvm --slp-schedule-budget=10] |
| 4 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --loop-idiom-vectorize-bytecmp-vf=8 -mllvm --slp-schedule-budget=10] |
| 5 | try_flags | 1.000x  [flags: -mllvm --partial-unrolling-threshold=32 -mllvm --loop-idiom-vectorize-bytecmp-vf=8 -mllvm --slp-schedule-budget=10] |
| 6 | try_flags | 1.006x  [flags: -mllvm -partial-unrolling-threshold=24] |
| 7 | try_flags | 1.007x  [flags: -mllvm -partial-unrolling-threshold=32 -mllvm -licm-max-num-uses-traversed=64] |
| 8 | try_flags | 1.001x  [flags: -mllvm -vectorize-memory-check-threshold=256] |
| 9 | try_flags | 1.008x  [flags: -mllvm -partial-unrolling-threshold=50] |

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>1.0332x</b>（基线 1126.16 ms，numeric 校验，3/3 次为正）</summary>

- 任务 `c4_pb007`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0506x　最终确认：**1.0332x**　IQR [1.0261, 1.0438]　base_cv=0.4% best_cv=0.6%
- 最终采纳编译选项：`-mllvm --partial-unrolling-threshold=64`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.032x  [flags: -mllvm --partial-unrolling-threshold=16] |
| 2 | try_flags | 1.041x  [flags: -mllvm -partial-unrolling-threshold=32] |
| 3 | try_flags | 1.028x  [flags: -mllvm -partial-unrolling-threshold=64] |
| 4 | try_flags | 1.005x  [flags: -mllvm -vectorize-memory-check-threshold=32] |
| 5 | try_flags | 1.016x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 6 | try_flags | 1.051x  [flags: -mllvm --partial-unrolling-threshold=64] |
| 7 | try_flags | 1.005x  [flags: -mllvm -vectorize-scev-check-threshold=8] |
| 8 | try_flags | 1.039x  [flags: -mllvm -partial-unrolling-threshold=64 -mllvm -slp-max-reg-size=128] |
| 9 | try_flags | 1.035x  [flags: -mllvm -partial-unrolling-threshold=64] |

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>1.0071x</b>（基线 1160.18 ms，numeric 校验，2/3 次为正）</summary>

- 任务 `c4_pb008`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.1560x　最终确认：**1.0071x**　IQR [0.9987, 1.0812]　base_cv=7.1% best_cv=7.4%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=32`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.148x  [flags: -mllvm -vectorize-memory-check-threshold=32] |
| 2 | try_flags | 1.156x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 3 | try_flags | 1.149x  [flags: -mllvm -licm-max-num-uses-traversed=128] |
| 4 | try_flags | 1.021x  [flags: -mllvm -vectorize-memory-check-threshold=64] |
| 5 | try_flags | 1.137x  [flags: -mllvm -partial-unrolling-threshold=400 -mllvm -licm-mssa-max-acc-promotion=2 -mllvm --instcombine-max-copied-from-constant-users=32] |
| 6 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 7 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 8 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |
| 9 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=32] |

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>1.0029x</b>（基线 3.11 ms，hash 校验，27/51 次为正）</summary>

- 任务 `c4_cb017`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.5847x　最终确认：**1.0029x**　IQR [0.9459, 1.0576]　base_cv=19.7% best_cv=19.4%
- 最终采纳编译选项：`-mllvm --aggressive-instcombine-max-scan-instrs=256`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.585x  [flags: -mllvm --aggressive-instcombine-max-scan-instrs=256] |
| 2 | try_flags | 1.393x  [flags: -mllvm -licm-max-num-uses-traversed=32 -mllvm -vectorize-memory-check-threshold=64] |
| 3 | try_flags | 1.000x  [flags: -mllvm --aggressive-instcombine-max-scan-instrs=256] |
| 4 | try_flags | 1.263x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 5 | try_flags | 1.000x  [flags: -mllvm --aggressive-instcombine-max-scan-instrs=256] |
| 6 | try_flags | 1.075x  [flags: -mllvm -slp-max-vf=4] |
| 7 | try_flags | 1.072x  [flags: -mllvm --pragma-unroll-full-max-iterations=256] |
| 8 | try_flags | 1.111x  [flags: -mllvm -slp-max-vf=0] |
| 9 |  | 1.387x  [flags: -mllvm -vectorize-memory-check-threshold=64] |

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>1.0000x</b>（基线 1.15 ms，hash 校验，0/0 次为正）</summary>

- 任务 `c4_cb018`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.000x  [无改善] |

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>1.0157x</b>（基线 5.94 ms，hash 校验，31/51 次为正）</summary>

- 任务 `c4_cb019`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：4.2294x　最终确认：**1.0157x**　IQR [0.9147, 1.1587]　base_cv=32.1% best_cv=34.3%
- 最终采纳编译选项：`-mllvm -licm-max-num-uses-traversed=16`
- 实际获得的反馈通道：`compiler`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 4.229x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 2 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=16] |
| 3 | try_flags | 2.770x  [flags: -mllvm -licm-mssa-optimization-cap=50] |
| 4 | try_flags | 2.815x  [flags: -mllvm -partial-unrolling-threshold=300] |
| 5 | try_flags | 2.801x  [flags: -mllvm -partial-unrolling-threshold=400] |
| 6 | try_flags | 3.285x  [flags: -mllvm -licm-max-num-uses-traversed=128] |
| 7 | try_flags | 1.934x  [flags: -mllvm -licm-max-num-uses-traversed=24 -mllvm -licm-mssa-optimization-cap=2 -mllvm -partial-unrolling-threshold=200] |
| 8 | try_flags | 2.864x  [flags: -mllvm -licm-max-num-uses-traversed=20] |
| 9 | try_flags | 3.642x  [flags: -mllvm -licm-max-num-int-reassociations=0] |

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>0.9625x</b>（基线 9.30 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c4_pb021`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：1.1273x　最终确认：**0.9625x**　IQR [0.9473, 1.0272]　base_cv=3.3% best_cv=4.0%
- 最终采纳编译选项：`-mllvm -partial-unrolling-threshold=100`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.127x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 4 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 5 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 6 | try_flags | 1.070x  [flags: -mllvm --unroll-max-iteration-count-to-analyze=128] |
| 7 | try_flags | 1.000x  [flags: -mllvm -licm-max-num-uses-traversed=64] |
| 8 | try_flags | 1.000x  [flags: -mllvm -partial-unrolling-threshold=100] |
| 9 | try_flags | 1.002x  [flags: -mllvm --slp-min-reg-size=5] |

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>0.9995x</b>（基线 524.19 ms，numeric 校验，1/3 次为正）</summary>

- 任务 `c4_pb009`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.0069x　最终确认：**0.9995x**　IQR [0.9985, 1.0007]　base_cv=0.1% best_cv=0.1%
- 最终采纳编译选项：`-mllvm -vectorize-scev-check-threshold=8`
- 实际获得的反馈通道：`compiler+hardware`

| 步 | 动作 | 该步实测 / 结果 |
|---:|---|---|
| 1 | try_flags | 1.000x  [无改善] |
| 2 | try_flags | 1.000x  [无改善] |
| 3 | try_flags | 1.000x  [无改善] |
| 4 | try_flags | 1.000x  [无改善] |
| 5 | try_flags | 1.000x  [无改善] |
| 6 | try_flags | 1.000x  [无改善] |
| 7 | try_flags | 1.000x  [无改善] |
| 8 | try_flags | 1.000x  [无改善] |
| 9 | try_flags | 1.007x  [flags: -mllvm -vectorize-scev-check-threshold=8] |

</details>

### OC — OpenCode + DeepSeek 外部通用 agent baseline（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>1.0000x</b>（基线 1261.77 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb010`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb010/round_*.jsonl`。

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>1.0000x</b>（基线 4760.78 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb011`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb011/round_*.jsonl`。

</details>

<details><summary><b>adi</b> — 中位加速比 <b>1.0000x</b>（基线 6589.41 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb025`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb025/round_*.jsonl`。

</details>

<details><summary><b>atax</b> — 中位加速比 <b>0.9759x</b>（基线 15.44 ms，? 校验，1/3 次为正）</summary>

- 任务 `oc_pb012`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.0042x　最终确认：**0.9759x**　IQR [0.9140, 1.0042]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb012/round_*.jsonl`。

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>3.4313x</b>（基线 14.05 ms，? 校验，35/35 次为正，⚠ 正确性门无效）</summary>

- 任务 `oc_cb001`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：4.2213x　最终确认：**3.4313x**　IQR [3.2154, 3.9199]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb001/round_*.jsonl`。

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>1.0045x</b>（基线 4.91 ms，? 校验，33/51 次为正）</summary>

- 任务 `oc_cb002`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1196x　最终确认：**1.0045x**　IQR [0.9938, 1.0130]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb002/round_*.jsonl`。

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>1.0000x</b>（基线 8.12 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_cb003`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb003/round_*.jsonl`。

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.0000x</b>（基线 61.99 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_cb004`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb004/round_*.jsonl`。

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>0.9994x</b>（基线 26.05 ms，? 校验，1/3 次为正）</summary>

- 任务 `oc_pb013`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.0340x　最终确认：**0.9994x**　IQR [0.9939, 1.0340]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb013/round_*.jsonl`。

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>1.0009x</b>（基线 48.96 ms，? 校验，7/11 次为正）</summary>

- 任务 `oc_cb005`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0190x　最终确认：**1.0009x**　IQR [0.9957, 1.0120]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb005/round_*.jsonl`。

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>1.0004x</b>（基线 86.13 ms，? 校验，4/7 次为正）</summary>

- 任务 `oc_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0079x　最终确认：**1.0004x**　IQR [0.9957, 1.0029]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb006/round_*.jsonl`。

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0000x</b>（基线 6409.99 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb016`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb016/round_*.jsonl`。

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>0.9946x</b>（基线 1.89 ms，? 校验，16/51 次为正）</summary>

- 任务 `oc_cb007`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.1965x　最终确认：**0.9946x**　IQR [0.9880, 1.0034]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb007/round_*.jsonl`。

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>1.0600x</b>（基线 2.61 ms，? 校验，42/51 次为正）</summary>

- 任务 `oc_cb008`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2107x　最终确认：**1.0600x**　IQR [1.0117, 1.1071]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb008/round_*.jsonl`。

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>0.9993x</b>（基线 1.36 ms，? 校验，23/51 次为正，⚠ 正确性门无效）</summary>

- 任务 `oc_cb009`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：2.4373x　最终确认：**0.9993x**　IQR [0.9886, 1.0204]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb009/round_*.jsonl`。

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>1.0005x</b>（基线 3.27 ms，? 校验，27/51 次为正）</summary>

- 任务 `oc_cb010`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3110x　最终确认：**1.0005x**　IQR [0.9855, 1.0131]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb010/round_*.jsonl`。

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>1.0000x</b>（基线 4875.85 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb001/round_*.jsonl`。

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>1.0000x</b>（基线 4912.74 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb002`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb002/round_*.jsonl`。

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>1.0000x</b>（基线 134.73 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb022`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb022/round_*.jsonl`。

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>1.0000x</b>（基线 241.39 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb014`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb014/round_*.jsonl`。

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.0177x</b>（基线 2.12 ms，? 校验，2/3 次为正）</summary>

- 任务 `oc_pb017`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.0256x　最终确认：**1.0177x**　IQR [0.9432, 1.0256]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb017/round_*.jsonl`。

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>1.0000x</b>（基线 469.20 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb026`，节点 `dgx-spark-a-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb026/round_*.jsonl`。

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>6.3561x</b>（基线 15849.83 ms，? 校验，3/3 次为正）</summary>

- 任务 `oc_pb023`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：6.3570x　最终确认：**6.3561x**　IQR [6.3548, 6.3570]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb023/round_*.jsonl`。

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>1.0000x</b>（基线 288.19 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb003`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb003/round_*.jsonl`。

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.0000x</b>（基线 36.22 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb004`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb004/round_*.jsonl`。

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>1.0000x</b>（基线 23.58 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb005`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb005/round_*.jsonl`。

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>1.0000x</b>（基线 1499.48 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb018`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb018/round_*.jsonl`。

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.0000x</b>（基线 1441.34 ms，? 校验，None/None 次为正，⚠ 正确性门无效）</summary>

- 任务 `oc_pb027`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb027/round_*.jsonl`。

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>1.1640x</b>（基线 1.15 ms，? 校验，3/3 次为正）</summary>

- 任务 `oc_pb028`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：1.1687x　最终确认：**1.1640x**　IQR [1.1531, 1.1687]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb028/round_*.jsonl`。

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>1.0000x</b>（基线 696.12 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb029`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb029/round_*.jsonl`。

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.0000x</b>（基线 7230.80 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb020`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb020/round_*.jsonl`。

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.0000x</b>（基线 34166.58 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb019`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb019/round_*.jsonl`。

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>1.0000x</b>（基线 18.48 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb015`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb015/round_*.jsonl`。

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>1.0038x</b>（基线 1.09 ms，? 校验，29/51 次为正）</summary>

- 任务 `oc_cb011`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：2.2992x　最终确认：**1.0038x**　IQR [0.9714, 1.0188]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb011/round_*.jsonl`。

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>1.0279x</b>（基线 1.06 ms，? 校验，37/51 次为正）</summary>

- 任务 `oc_cb012`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：2.4038x　最终确认：**1.0279x**　IQR [0.9406, 1.0614]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb012/round_*.jsonl`。

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>6.5494x</b>（基线 3695.28 ms，? 校验，3/3 次为正）</summary>

- 任务 `oc_pb024`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：6.5922x　最终确认：**6.5494x**　IQR [6.5416, 6.5922]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb024/round_*.jsonl`。

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>1.0003x</b>（基线 2.33 ms，? 校验，26/51 次为正）</summary>

- 任务 `oc_cb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.9765x　最终确认：**1.0003x**　IQR [0.9795, 1.0361]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb013/round_*.jsonl`。

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>1.0242x</b>（基线 1.55 ms，? 校验，44/51 次为正）</summary>

- 任务 `oc_cb021`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0521x　最终确认：**1.0242x**　IQR [1.0160, 1.0284]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb021/round_*.jsonl`。

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>1.0000x</b>（基线 1.75 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_cb020`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb020/round_*.jsonl`。

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>1.0000x</b>（基线 2.06 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_cb014`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb014/round_*.jsonl`。

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>0.9734x</b>（基线 0.91 ms，? 校验，19/51 次为正）</summary>

- 任务 `oc_cb015`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.3306x　最终确认：**0.9734x**　IQR [0.9417, 1.0787]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb015/round_*.jsonl`。

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>0.9934x</b>（基线 2.02 ms，? 校验，20/51 次为正）</summary>

- 任务 `oc_cb016`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.3055x　最终确认：**0.9934x**　IQR [0.9696, 1.0151]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb016/round_*.jsonl`。

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>1.0000x</b>（基线 13396.17 ms，? 校验，None/None 次为正，⚠ 正确性门无效）</summary>

- 任务 `oc_pb030`，节点 `dgx-spark-b-2`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb030/round_*.jsonl`。

</details>

<details><summary><b>symm</b> — 中位加速比 <b>1.0000x</b>（基线 4053.44 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb006`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb006/round_*.jsonl`。

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>1.0000x</b>（基线 1197.51 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb007`，节点 `dgx-spark-a-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb007/round_*.jsonl`。

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>1.0000x</b>（基线 393.71 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb008`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb008/round_*.jsonl`。

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>1.1096x</b>（基线 2.13 ms，? 校验，48/51 次为正）</summary>

- 任务 `oc_cb017`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.8189x　最终确认：**1.1096x**　IQR [1.0971, 1.1192]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb017/round_*.jsonl`。

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>1.0054x</b>（基线 1.29 ms，? 校验，27/51 次为正）</summary>

- 任务 `oc_cb018`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.2745x　最终确认：**1.0054x**　IQR [0.9730, 1.0563]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb018/round_*.jsonl`。

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>1.0084x</b>（基线 0.87 ms，? 校验，28/51 次为正）</summary>

- 任务 `oc_cb019`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：2.3696x　最终确认：**1.0084x**　IQR [0.7196, 1.4134]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_cb019/round_*.jsonl`。

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>1.0254x</b>（基线 9.83 ms，? 校验，3/3 次为正）</summary>

- 任务 `oc_pb021`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：1.0990x　最终确认：**1.0254x**　IQR [1.0240, 1.0990]

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb021/round_*.jsonl`。

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>1.0000x</b>（基线 542.05 ms，? 校验，None/None 次为正）</summary>

- 任务 `oc_pb009`，节点 `dgx-spark-b-1`，数据源 `arch`
- 探索期最好单次：0.0000x　最终确认：**1.0000x**

外部 agent 共交互 9 轮；该 harness 只在最终确认阶段记录结果，逐轮原始输出见节点上的 `opencode_runs/oc_pb009/round_*.jsonl`。

</details>

### PO — AutoPass (arXiv 2606.20373) 四-agent 复现 baseline（51 个程序）

<details><summary><b>2mm</b> — 中位加速比 <b>1.0138x</b>（基线 3757.63 ms，? 校验，2/3 次为正）</summary>

- 任务 `po_pb010`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0238x　最终确认：**1.0138x**　IQR [0.9931, 1.0150]
- 最终 pass 顺序（13 个）：`loop-rotate,loop-mssa(licm),loop-mssa(lnicm),indvars,loop-vectorize,loop-unroll,instcombine,simplifycfg,loop-mssa(licm),loop-vectorize,loop-unroll,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=800 -unroll-count=4 -unroll-partial-threshold=300 -unroll-max-count=8 -force-target-max-vector-interleave=2 -vectorizer-min-trip-count=4 -licm-max-num-uses-traversed=128`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9945x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |
| 2/3 | speedup=1.0157x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128'] |
| 3/3 | speedup=1.0238x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=800', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128'] |

</details>

<details><summary><b>3mm</b> — 中位加速比 <b>0.9954x</b>（基线 3723.74 ms，? 校验，1/3 次为正）</summary>

- 任务 `po_pb011`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0211x　最终确认：**0.9954x**　IQR [0.9343, 1.0639]
- 最终 pass 顺序（12 个）：`loop-rotate,loop-mssa(licm),indvars,loop-unroll,loop-vectorize,loop-unroll,loop-vectorize,slp-vectorizer,loop-distribute,loop-unroll,loop-vectorize,slp-vectorizer`
- 最终 pass 参数：`-unroll-threshold=300 -unroll-count=2 -vectorizer-min-trip-count=4 -slp-threshold=-5 -force-target-max-vector-interleave=2`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9657x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-simplifycfg', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=0'] |
| 2/3 | speedup=1.0211x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-distribute', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=300', '-unroll-count=2', '-vectorizer-min-trip-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=2'] |
| 3/3 | speedup=0.8937x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-distribute', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=2'] |

</details>

<details><summary><b>adi</b> — 中位加速比 <b>1.0000x</b>（基线 9358.96 ms，? 校验，1/3 次为正）</summary>

- 任务 `po_pb025`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**　IQR [0.9997, 1.0004]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8828x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'loop-versioning-licm', 'loop-unroll', 'loop-unroll-full', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'instcombine', 'aggressive-instcombine', 'gvn', 'gvn-hoist', 'reassociate', 'nary-reassociate', 'slsr', 'separate-const-offset-from-gep', 'sroa', 'early-cse', 'dce', 'bdce', 'instsimplify', 'simplifycfg', 'loop-simplifycfg', 'loop-deletion', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=300', '-unroll |
| 2/3 | speedup=0.7431x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-versioning-licm', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'reassociate', 'nary-reassociate', 'slsr', 'separate-const-offset-from-gep', 'gvn', 'gvn-hoist', 'early-cse', 'dce', 'bdce', 'instsimplify', 'simplifycfg', 'loop-simplifycfg', 'loop-deletion', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=600', '-unroll-max-count=16', '-slp-threshold=-10', '-vectorizer-min-tri |
| 3/3 | speedup=0.7486x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'reassociate', 'nary-reassociate', 'slsr', 'separate-const-offset-from-gep', 'gvn', 'gvn-hoist', 'early-cse', 'dce', 'bdce', 'instsimplify', 'simplifycfg', 'loop-simplifycfg', 'loop-deletion', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=150', '-unroll-max-count=4', '-slp-threshold=0', '-vectorizer-min-trip-count |

</details>

<details><summary><b>atax</b> — 中位加速比 <b>1.0007x</b>（基线 22.14 ms，? 校验，11/21 次为正）</summary>

- 任务 `po_pb012`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0007x**　IQR [0.9826, 1.0249]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.7124x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'indvars', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'slp-vectorizer', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'gvn', 'gvn-hoist', 'gvn-sink', 'simplifycfg', 'adce', 'bdce', 'dse', 'instcombine'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=0', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=0.7776x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'indvars', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'slp-vectorizer', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'gvn', 'gvn-hoist', 'gvn-sink', 'simplifycfg', 'adce', 'bdce', 'dse', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=0.7221x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'instcombine', 'simplifycfg', 'indvars', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'instcombine', 'gvn', 'gvn-hoist', 'gvn-sink', 'simplifycfg', 'adce', 'bdce', 'dse', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |

</details>

<details><summary><b>automotive_qsort1</b> — 中位加速比 <b>0.9913x</b>（基线 14.56 ms，? 校验，9/35 次为正，⚠ 正确性门无效）</summary>

- 任务 `po_cb001`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0154x　最终确认：**0.9913x**　IQR [0.9651, 1.0038]
- 最终 pass 顺序（21 个）：`sroa,early-cse,simplifycfg,instcombine,gvn,correlated-propagation,indvars,loop-rotate,loop-mssa(licm),loop-unroll,loop-unroll,loop-unroll,simplifycfg,instcombine,adce,bdce,dse,loop-vectorize,instcombine,simplifycfg,sroa`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -unroll-max-count=8 -vectorizer-min-trip-count=8 -licm-max-num-uses-traversed=32`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0099x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'indvars', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'simplifycfg', 'instcombine', 'adce', 'bdce', 'dse', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8', '-vectorizer-min-trip-count=8', '-licm-max-num-uses-traversed=32'] |
| 2/3 | speedup=1.0154x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'indvars', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'simplifycfg', 'instcombine', 'adce', 'bdce', 'dse', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-vectorizer-min-trip-count=8', '-licm-max-num-uses-traversed=32'] |
| 3/3 | speedup=1.0079x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'indvars', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'simplifycfg', 'instcombine', 'adce', 'bdce', 'dse', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-unroll-max-count=16', '-vectorizer-min-trip-count=8', '-licm-max-num-uses-traversed=32'] |

</details>

<details><summary><b>automotive_susan_corners</b> — 中位加速比 <b>0.9957x</b>（基线 4.94 ms，? 校验，23/51 次为正）</summary>

- 任务 `po_cb002`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0255x　最终确认：**0.9957x**　IQR [0.9703, 1.0086]
- 最终 pass 顺序（25 个）：`simplifycfg,sroa,early-cse,instcombine,jump-threading,correlated-propagation,gvn,gvn-hoist,instcombine,loop-rotate,loop-mssa(licm),indvars,loop-unroll,loop-vectorize,loop-unroll,slp-vectorizer,vector-combine,instcombine,gvn,dse,mldst-motion,loop-distribute,loop-versioning-licm,sink,aggressive-instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -slp-threshold=-5 -force-target-max-vector-interleave=4 -vectorizer-min-trip-count=4 -gvn-max-num-deps=200 -licm-max-num-uses-traversed=128`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0255x  ACCEPTED (new P*)  passes=['simplifycfg', 'sroa', 'early-cse', 'instcombine', 'jump-threading', 'correlated-propagation', 'gvn', 'gvn-hoist', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'vector-combine', 'instcombine', 'gvn', 'dse', 'mldst-motion', 'loop-distribute', 'loop-versioning-licm', 'sink', 'aggressive-instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-gvn-max-num-deps=200', '-li |
| 2/3 | speedup=0.9616x  REJECTED, rollback to P*  passes=['simplifycfg', 'sroa', 'early-cse', 'instcombine', 'jump-threading', 'correlated-propagation', 'gvn', 'gvn-hoist', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'vector-combine', 'instcombine', 'gvn', 'dse', 'mldst-motion', 'loop-distribute', 'loop-versioning-licm', 'sink', 'aggressive-instcombine', 'slp-vectorizer', 'vector-combine', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '- |
| 3/3 | speedup=0.9878x  REJECTED, rollback to P*  passes=['simplifycfg', 'sroa', 'early-cse', 'instcombine', 'jump-threading', 'correlated-propagation', 'gvn', 'gvn-hoist', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'vector-combine', 'instcombine', 'gvn', 'dse', 'mldst-motion', 'loop-distribute', 'loop-versioning-licm', 'sink', 'aggressive-instcombine', 'slp-vectorizer', 'vector-combine', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-v |

</details>

<details><summary><b>automotive_susan_edges</b> — 中位加速比 <b>0.9780x</b>（基线 8.10 ms，? 校验，0/51 次为正）</summary>

- 任务 `po_cb003`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0137x　最终确认：**0.9780x**　IQR [0.9761, 0.9801]
- 最终 pass 顺序（22 个）：`sroa,early-cse,instcombine,simplifycfg,gvn,loop-mssa(licm),loop-unroll,loop-unroll,instcombine,slp-vectorizer,vector-combine,instcombine,simplifycfg,gvn,loop-unroll,slp-vectorizer,instcombine,gvn-hoist,loop-mssa(licm),loop-unroll,slp-vectorizer,instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -slp-threshold=-5 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0109x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg', 'gvn', 'loop-unroll', 'slp-vectorizer'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.0137x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg', 'gvn', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'gvn-hoist', 'loop-mssa(licm)', 'loop-unroll', 'slp-vectorizer', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 3/3 | speedup=1.0036x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg', 'gvn', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'gvn-hoist', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'vector-combine', 'instcombine', 'gvn', 'loop-mssa(licm)', 'slp-vectorizer', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8', '-gvn-max-num-deps=200'] |

</details>

<details><summary><b>automotive_susan_smoothing</b> — 中位加速比 <b>1.0011x</b>（基线 61.61 ms，? 校验，6/9 次为正）</summary>

- 任务 `po_cb004`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0011x**　IQR [0.9990, 1.0025]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9328x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'newgvn', 'instcombine', 'slp-vectorizer', 'instcombine', 'early-cse', 'simplifycfg', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-mssa(licm)', 'loop-vectorize', 'instcombine', 'slp-vectorizer', 'gvn'] params=['-slp-threshold=-5', '-unroll-threshold=300', '-unroll-count=4', '-inline-threshold=400'] |
| 2/3 | speedup=0.6030x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'newgvn', 'instcombine', 'slp-vectorizer', 'instcombine', 'early-cse', 'simplifycfg', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-mssa(licm)', 'loop-vectorize', 'instcombine', 'slp-vectorizer', 'gvn'] params=['-slp-threshold=-10', '-unroll-threshold=600', '-unroll-count=4', '-inline-threshold=800'] |
| 3/3 | speedup=0.9587x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'newgvn', 'instcombine', 'slp-vectorizer', 'instcombine', 'early-cse', 'simplifycfg', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-mssa(licm)', 'loop-vectorize', 'instcombine', 'slp-vectorizer', 'gvn'] params=['-slp-threshold=0', '-unroll-threshold=150', '-unroll-count=2', '-inline-threshold=225'] |

</details>

<details><summary><b>bicg</b> — 中位加速比 <b>0.9960x</b>（基线 34.36 ms，? 校验，6/15 次为正）</summary>

- 任务 `po_pb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**0.9960x**　IQR [0.9901, 1.0274]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8173x  REJECTED, rollback to P*  passes=['typepromotion', 'sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-slp-threshold=0', '-inline-threshold=225'] |
| 2/3 | speedup=0.8671x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=8', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=4', '-slp-threshold=-5', '-inline-threshold=400'] |
| 3/3 | speedup=0.8979x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=8', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-slp-threshold=-5', '-inline-threshold=400'] |

</details>

<details><summary><b>bzip2_decode</b> — 中位加速比 <b>1.0038x</b>（基线 49.64 ms，? 校验，9/11 次为正）</summary>

- 任务 `po_cb005`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0038x**　IQR [1.0019, 1.0179]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9932x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'gvn', 'newgvn', 'slp-vectorizer', 'loop-vectorize', 'instcombine', 'simplifycfg', 'jump-threading', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'gvn', 'loop-mssa(licm)', 'loop-vectorize', 'instcombine', 'simplifycfg', 'adce', 'dse', 'dce'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-gvn-max-num-deps=200'] |
| 2/3 | speedup=0.9955x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'gvn', 'loop-mssa(licm)', 'jump-threading', 'simplifycfg', 'adce', 'dce', 'dse'] params=['-unroll-threshold=300', '-unroll-count=2', '-slp-threshold=0', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=1', '-gvn-max-num-deps=200'] |
| 3/3 | speedup=0.9971x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'gvn', 'loop-mssa(licm)', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'gvn', 'simplifycfg', 'adce', 'dce', 'dse'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-gvn-max-num-deps=200'] |

</details>

<details><summary><b>bzip2_encode</b> — 中位加速比 <b>0.9998x</b>（基线 85.74 ms，? 校验，3/7 次为正）</summary>

- 任务 `po_cb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0047x　最终确认：**0.9998x**　IQR [0.9961, 1.0027]
- 最终 pass 顺序（24 个）：`loop-rotate,loop-mssa(licm),loop-idiom,gvn,instcombine,simplifycfg,slp-vectorizer,loop-vectorize,loop-unroll,loop-unroll-full,instcombine,simplifycfg,loop-mssa(licm),gvn,dse,mldst-motion,slp-vectorizer,loop-vectorize,loop-unroll,instcombine,simplifycfg,slp-vectorizer,loop-vectorize,loop-unroll`
- 最终 pass 参数：`-slp-threshold=-10 -unroll-threshold=1600 -unroll-count=8 -vectorizer-min-trip-count=4 -force-target-max-vector-interleave=4`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0007x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'dse', 'mldst-motion', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll'] params=['-slp-threshold=-5', '-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.0025x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'dse', 'mldst-motion', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll'] params=['-slp-threshold=-10', '-unroll-threshold=1200', '-unroll-count=8', '-vectorizer-min-trip-count=8'] |
| 3/3 | speedup=1.0047x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-idiom', 'gvn', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'gvn', 'dse', 'mldst-motion', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll'] params=['-slp-threshold=-10', '-unroll-threshold=1600', '-unroll-count=8', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4'] |

</details>

<details><summary><b>cholesky</b> — 中位加速比 <b>1.0165x</b>（基线 26650.26 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb016`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0110x　最终确认：**1.0165x**　IQR [1.0062, 1.0167]
- 最终 pass 顺序（25 个）：`sroa,early-cse,loop-mssa(licm),gvn,loop-rotate,loop-vectorize,loop-unroll,instcombine,simplifycfg,loop-mssa(licm),gvn,loop-vectorize,loop-unroll,instcombine,simplifycfg,sroa,early-cse,loop-mssa(licm),gvn,loop-rotate,loop-vectorize,loop-unroll,instcombine,loop-mssa(licm),gvn`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -vectorizer-min-trip-count=4 -licm-max-num-uses-traversed=128 -slp-threshold=-5 -jump-threading-threshold=12 -gvn-max-num-deps=200`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0099x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'gvn', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128'] |
| 2/3 | speedup=1.0108x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'gvn', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-slp-threshold=0'] |
| 3/3 | speedup=1.0110x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'gvn', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'sroa', 'early-cse', 'loop-mssa(licm)', 'gvn', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'loop-mssa(licm)', 'gvn'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-slp-threshold=-5', '-jump-threading-threshold=12', '-gvn-max-num-deps=200'] |

</details>

<details><summary><b>consumer_tiff2bw</b> — 中位加速比 <b>0.9978x</b>（基线 2.77 ms，? 校验，24/51 次为正）</summary>

- 任务 `po_cb007`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0177x　最终确认：**0.9978x**　IQR [0.8018, 1.2011]
- 最终 pass 顺序（12 个）：`loop-rotate,loop-mssa(licm),gvn,mldst-motion,loop-unroll,loop-unroll,instcombine,simplifycfg,loop-vectorize,slp-vectorizer,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=150 -unroll-count=2 -slp-threshold=0 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9551x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'dse', 'mldst-motion', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=0.8156x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'dse', 'mldst-motion', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-mssa(licm)'] params=['-unroll-threshold=600', '-unroll-count=8', '-slp-threshold=-5', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=1.0177x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'gvn', 'mldst-motion', 'loop-unroll', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=150', '-unroll-count=2', '-slp-threshold=0', '-vectorizer-min-trip-count=8'] |

</details>

<details><summary><b>consumer_tiff2dither</b> — 中位加速比 <b>0.9938x</b>（基线 2.52 ms，? 校验，23/51 次为正）</summary>

- 任务 `po_cb008`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0335x　最终确认：**0.9938x**　IQR [0.9508, 1.1479]
- 最终 pass 顺序（24 个）：`sroa,early-cse,simplifycfg,instcombine,gvn,correlated-propagation,jump-threading,adce,bdce,dse,memcpyopt,mldst-motion,loop-rotate,loop-mssa(licm),indvars,loop-unroll,loop-unroll-full,loop-deletion,loop-simplifycfg,sccp,instsimplify,reassociate,slsr,separate-const-offset-from-gep`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -inline-threshold=225 -licm-max-num-uses-traversed=32`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9300x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'jump-threading', 'adce', 'bdce', 'dse', 'memcpyopt', 'mldst-motion', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-unroll-full', 'loop-deletion', 'loop-simplifycfg', 'sccp', 'instsimplify', 'reassociate', 'slsr', 'separate-const-offset-from-gep'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-inline-threshold=225', '-licm-max-num-uses-traversed=32'] |
| 2/3 | speedup=0.9440x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'jump-threading', 'adce', 'bdce', 'dse', 'memcpyopt', 'mldst-motion', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-unroll-full', 'loop-deletion', 'loop-simplifycfg', 'sccp', 'instsimplify', 'reassociate', 'slsr', 'separate-const-offset-from-gep'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=50', '-inline-threshold=225', '-licm-max-num-uses-traversed=32'] |
| 3/3 | speedup=1.0335x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'correlated-propagation', 'jump-threading', 'adce', 'bdce', 'dse', 'memcpyopt', 'mldst-motion', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-unroll-full', 'loop-deletion', 'loop-simplifycfg', 'sccp', 'instsimplify', 'reassociate', 'slsr', 'separate-const-offset-from-gep'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-inline-threshold=225', '-licm-max-num-uses-traversed=32'] |

</details>

<details><summary><b>consumer_tiff2median</b> — 中位加速比 <b>1.0006x</b>（基线 2.10 ms，? 校验，27/51 次为正，⚠ 正确性门无效）</summary>

- 任务 `po_cb009`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0252x　最终确认：**1.0006x**　IQR [0.7270, 1.0139]
- 最终 pass 顺序（19 个）：`sroa,early-cse,simplifycfg,jump-threading,instcombine,gvn,correlated-propagation,adce,bdce,dse,loop-rotate,indvars,loop-mssa(licm),loop-unroll,loop-unroll,loop-vectorize,instcombine,simplifycfg,sroa`
- 最终 pass 参数：`-unroll-threshold=300 -unroll-count=4 -jump-threading-threshold=12 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0252x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'jump-threading', 'instcombine', 'gvn', 'correlated-propagation', 'adce', 'bdce', 'dse', 'loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa'] params=['-unroll-threshold=300', '-unroll-count=4', '-jump-threading-threshold=12', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.0063x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'jump-threading', 'instcombine', 'gvn', 'correlated-propagation', 'adce', 'bdce', 'dse', 'loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa'] params=['-unroll-threshold=600', '-unroll-count=8', '-jump-threading-threshold=24', '-vectorizer-min-trip-count=8'] |
| 3/3 | speedup=1.0167x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'jump-threading', 'instcombine', 'gvn', 'correlated-propagation', 'adce', 'bdce', 'dse', 'loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'instcombine', 'simplifycfg', 'sroa', 'gvn-sink', 'loop-versioning-licm', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-jump-threading-threshold=12', '-vectorizer-min-trip-count=8', '-licm-max-num-uses-traversed=32', '-gvn-max-num-deps=100'] |

</details>

<details><summary><b>consumer_tiff2rgba</b> — 中位加速比 <b>0.9602x</b>（基线 3.29 ms，? 校验，1/51 次为正）</summary>

- 任务 `po_cb010`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0130x　最终确认：**0.9602x**　IQR [0.9483, 0.9768]
- 最终 pass 顺序（11 个）：`instcombine,simplifycfg,jump-threading,correlated-propagation,simplifycfg,instcombine,gvn,adce,dse,instcombine,simplifycfg`
- 最终 pass 参数：`-jump-threading-threshold=3`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9731x  REJECTED, rollback to P*  passes=['simplifycfg', 'instcombine', 'sroa', 'early-cse', 'jump-threading', 'correlated-propagation', 'simplifycfg', 'instcombine', 'gvn', 'adce', 'bdce', 'dse', 'instcombine', 'simplifycfg'] params=['-jump-threading-threshold=6'] |
| 2/3 | speedup=0.9746x  REJECTED, rollback to P*  passes=['sroa', 'instcombine', 'early-cse', 'simplifycfg', 'jump-threading', 'correlated-propagation', 'simplifycfg', 'instcombine', 'gvn', 'adce', 'bdce', 'dse', 'instcombine', 'simplifycfg'] params=['-jump-threading-threshold=12'] |
| 3/3 | speedup=1.0130x  ACCEPTED (new P*)  passes=['instcombine', 'simplifycfg', 'jump-threading', 'correlated-propagation', 'simplifycfg', 'instcombine', 'gvn', 'adce', 'dse', 'instcombine', 'simplifycfg'] params=['-jump-threading-threshold=3'] |

</details>

<details><summary><b>correlation</b> — 中位加速比 <b>1.0000x</b>（基线 4872.56 ms，? 校验，None/None 次为正）</summary>

- 任务 `po_pb001`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | FAILED: incorrect: output hash mismatch (ref=3767002ed932, opt=3d198762055c) |
| 2/3 | FAILED: incorrect: output hash mismatch (ref=3767002ed932, opt=3d198762055c) |
| 3/3 | FAILED: incorrect: output hash mismatch (ref=3767002ed932, opt=3d198762055c) |

</details>

<details><summary><b>covariance</b> — 中位加速比 <b>0.9897x</b>（基线 4868.66 ms，? 校验，0/3 次为正）</summary>

- 任务 `po_pb002`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**0.9897x**　IQR [0.9871, 0.9942]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9602x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'loop-mssa(licm)', 'instcombine', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2'] |
| 2/3 | speedup=0.9965x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4'] |
| 3/3 | speedup=0.9950x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=600', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-f |

</details>

<details><summary><b>deriche</b> — 中位加速比 <b>1.0037x</b>（基线 238.47 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb022`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0067x　最终确认：**1.0037x**　IQR [1.0019, 1.0070]
- 最终 pass 顺序（19 个）：`sroa,early-cse,instcombine,simplifycfg,gvn,loop-mssa(licm),loop-rotate,indvars,loop-unroll,loop-unroll,loop-unroll,loop-vectorize,slp-vectorizer,slp-vectorizer,slp-vectorizer,load-store-vectorizer,instcombine,simplifycfg,dce`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=8 -unroll-partial-threshold=300 -slp-threshold=-10 -force-target-max-vector-interleave=4 -vectorizer-min-trip-count=4 -gvn-max-num-deps=200`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9993x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'dce'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4'] |
| 2/3 | speedup=1.0067x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'slp-vectorizer', 'load-store-vectorizer', 'instcombine', 'simplifycfg', 'dce'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-gvn-max-num-deps=200'] |
| 3/3 | speedup=1.0067x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'gvn-sink', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'slp-vectorizer', 'slp-vectorizer', 'load-store-vectorizer', 'instcombine', 'simplifycfg', 'dce'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-gvn-max-num-deps=200'] |

</details>

<details><summary><b>doitgen</b> — 中位加速比 <b>1.0222x</b>（基线 558.20 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb014`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0236x　最终确认：**1.0222x**　IQR [1.0156, 1.0235]
- 最终 pass 顺序（22 个）：`loop-rotate,loop-mssa(licm),loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,loop-unroll,loop-unroll,slp-vectorizer,slp-vectorizer,gvn,gvn-hoist,dse,dce,instcombine,simplifycfg,loop-unroll,loop-unroll,loop-vectorize,slp-vectorizer,instcombine,dce`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -unroll-max-count=8 -slp-threshold=-5 -vectorizer-min-trip-count=4 -force-target-max-vector-interleave=4 -inline-threshold=400`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0236x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'slp-vectorizer', 'gvn', 'gvn-hoist', 'dse', 'dce', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'dce'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-inline-threshold=400'] |
| 2/3 | speedup=1.0088x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'slp-vectorizer', 'gvn', 'gvn-hoist', 'dse', 'dce', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'dce', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-f |
| 3/3 | speedup=1.0205x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'gvn', 'gvn-hoist', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'dce', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'dce'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4',  |

</details>

<details><summary><b>durbin</b> — 中位加速比 <b>1.0199x</b>（基线 3.81 ms，? 校验，47/51 次为正）</summary>

- 任务 `po_pb017`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0274x　最终确认：**1.0199x**　IQR [1.0083, 1.0291]
- 最终 pass 顺序（10 个）：`gvn,instcombine,gvn-hoist,gvn-sink,memcpyopt,mldst-motion,slp-vectorizer,loop-vectorize,loop-unroll,instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -slp-threshold=-5 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0274x  ACCEPTED (new P*)  passes=['gvn', 'instcombine', 'gvn-hoist', 'gvn-sink', 'memcpyopt', 'mldst-motion', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=0.9913x  REJECTED, rollback to P*  passes=['gvn', 'instcombine', 'gvn-hoist', 'gvn-sink', 'memcpyopt', 'mldst-motion', 'loop-rotate', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine', 'gvn'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-10', '-vectorizer-min-trip-count=8'] |
| 3/3 | speedup=0.9946x  REJECTED, rollback to P*  passes=['gvn', 'instcombine', 'gvn-hoist', 'gvn-sink', 'memcpyopt', 'mldst-motion', 'loop-rotate', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |

</details>

<details><summary><b>fdtd-2d</b> — 中位加速比 <b>1.0294x</b>（基线 916.82 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb026`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0183x　最终确认：**1.0294x**　IQR [1.0082, 1.0300]
- 最终 pass 顺序（21 个）：`sroa,instcombine,simplifycfg,loop-rotate,loop-data-prefetch,loop-vectorize,slp-vectorizer,load-store-vectorizer,vector-combine,loop-unroll,loop-unroll,loop-unroll,instcombine,simplifycfg,loop-rotate,loop-vectorize,slp-vectorizer,load-store-vectorizer,vector-combine,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=150 -unroll-count=4 -force-target-max-vector-interleave=4 -slp-threshold=-10 -vectorizer-min-trip-count=4 -unroll-partial-threshold=150 -unroll-max-count=4`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9173x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'slp-vectorizer', 'load-store-vectorizer', 'vector-combine', 'loop-unroll', 'loop-unroll', 'loop-reduce', 'loop-rotate', 'loop-vectorize', 'slp-vectorizer', 'load-store-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-force-target-max-vector-interleave=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=0.9340x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'slp-vectorizer', 'load-store-vectorizer', 'vector-combine', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'loop-vectorize', 'load-store-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-unroll', 'slp-vectorizer', 'loop-vectorize', 'load-store-vectorizer', 'vector-combine', 'instcombine'] params=['-unroll-threshold=300', '-unroll-count=2', '-force-target-max-vector-interleave=2', '-slp-threshold=0', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=1.0183x  ACCEPTED (new P*)  passes=['sroa', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-data-prefetch', 'loop-vectorize', 'slp-vectorizer', 'load-store-vectorizer', 'vector-combine', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-vectorize', 'slp-vectorizer', 'load-store-vectorizer', 'vector-combine', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=150', '-unroll-count=4', '-force-target-max-vector-interleave=4', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=4'] |

</details>

<details><summary><b>floyd-warshall</b> — 中位加速比 <b>1.0000x</b>（基线 15851.03 ms，? 校验，None/None 次为正）</summary>

- 任务 `po_pb023`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | FAILED: incorrect: output hash mismatch (ref=780732d4fd60, opt=c948790b569c) |
| 2/3 | FAILED: incorrect: output hash mismatch (ref=780732d4fd60, opt=c948790b569c) |
| 3/3 | FAILED: incorrect: output hash mismatch (ref=780732d4fd60, opt=c948790b569c) |

</details>

<details><summary><b>gemm</b> — 中位加速比 <b>0.9994x</b>（基线 291.03 ms，? 校验，1/3 次为正）</summary>

- 任务 `po_pb003`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**0.9994x**　IQR [0.9938, 1.0053]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.2923x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-jump-threading-threshold=6', '-gvn-max-num-deps=100'] |
| 2/3 | speedup=0.2956x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-jump-threading-threshold=6', '-gvn-max-num-deps=100'] |
| 3/3 | speedup=0.2954x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'instcombine'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8', '-slp-threshold=0', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-jump-threading-threshold=6', '-gvn-max-num-deps=100'] |

</details>

<details><summary><b>gemver</b> — 中位加速比 <b>1.0004x</b>（基线 37.34 ms，? 校验，8/15 次为正）</summary>

- 任务 `po_pb004`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0004x**　IQR [0.9880, 1.0043]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.6455x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'instcombine', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-gvn-max-num-deps=100'] |
| 2/3 | speedup=0.6461x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'instcombine', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=0', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-gvn-max-num-deps=200'] |
| 3/3 | speedup=0.6440x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'instcombine', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=300', '-unroll-count=2', '-unroll-partial-threshold=150', '-slp-threshold=-10', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=2', '-gvn-max-num-deps=50', '-jump-threading-threshold=6'] |

</details>

<details><summary><b>gesummv</b> — 中位加速比 <b>0.9989x</b>（基线 21.73 ms，? 校验，10/23 次为正）</summary>

- 任务 `po_pb005`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**0.9989x**　IQR [0.9855, 1.0177]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8671x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-u |
| 2/3 | speedup=0.8287x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'loop-unroll'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-slp-threshold=0'] |
| 3/3 | speedup=0.8600x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'loop-unroll-full', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=100', '-unroll-max-count=4', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=2', '-slp-threshold=-5'] |

</details>

<details><summary><b>gramschmidt</b> — 中位加速比 <b>0.9947x</b>（基线 6904.30 ms，? 校验，1/3 次为正）</summary>

- 任务 `po_pb018`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0526x　最终确认：**0.9947x**　IQR [0.9828, 1.0116]
- 最终 pass 顺序（20 个）：`loop-rotate,indvars,loop-mssa(licm),loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,slp-vectorizer,slp-vectorizer,loop-simplifycfg,loop-distribute,loop-unroll,loop-unroll,loop-vectorize,slp-vectorizer,instcombine,simplifycfg,gvn,dse,adce`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -slp-threshold=-5 -force-target-max-vector-interleave=4 -vectorizer-min-trip-count=4`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0526x  ACCEPTED (new P*)  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'loop-simplifycfg', 'loop-distribute', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |
| 2/3 | speedup=1.0131x  REJECTED, rollback to P*  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'loop-simplifycfg', 'loop-distribute', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=1200', '-unroll-count=8', '-unroll-partial-threshold=600', '-slp-threshold=-10', '-force-target-max-vector-interleave=8', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=0.9372x  REJECTED, rollback to P*  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'loop-simplifycfg', 'loop-distribute', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=800', '-unroll-count=6', '-unroll-partial-threshold=400', '-slp-threshold=-3', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |

</details>

<details><summary><b>heat-3d</b> — 中位加速比 <b>1.0019x</b>（基线 4734.52 ms，? 校验，2/3 次为正，⚠ 正确性门无效、孤儿抢核、PO 预算被 InstCombine 吞）</summary>

- 任务 `po_pb027`，节点 `dgx-spark-b-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0019x**　IQR [0.9970, 1.0023]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9614x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'loop-unroll', 'simplifycfg', 'instcombine', 'gvn', 'slp-vectorizer', 'instcombine', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'simplifycfg', 'instcombine', 'gvn', 'slp-vectorizer', 'instcombine'] params=['-force-target-max-vector-interleave=4', '-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5'] |
| 2/3 | speedup=0.7331x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'instcombine', 'gvn', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'loop-rotate', 'loop-vectorize', 'instcombine', 'gvn', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'adce'] params=['-unroll-threshold=600', '-unroll-count=8', '-slp-threshold=0', '-force-target-max-vector-interleave=8', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=0.9594x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'instcombine', 'gvn', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'simplifycfg', 'gvn', 'loop-rotate', 'loop-vectorize', 'instcombine', 'gvn', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'simplifycfg', 'adce', 'instcombine'] params=['-unroll-threshold=150', '-unroll-count=2', '-slp-threshold=-10', '-force-target-max-vector-interleave=1', '-vectorizer-min-trip-count=8'] |

</details>

<details><summary><b>jacobi-1d</b> — 中位加速比 <b>1.0022x</b>（基线 2.15 ms，? 校验，31/51 次为正）</summary>

- 任务 `po_pb028`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0022x**　IQR [0.9759, 1.0118]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9241x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=2', '-licm-max-num-uses-traversed=32'] |
| 2/3 | speedup=0.7333x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-unroll', 'loop-vectorize', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=0', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=4', '-licm-max-num-uses-traversed=128'] |
| 3/3 | speedup=0.6934x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=2', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-licm-max-num-uses-traversed=32'] |

</details>

<details><summary><b>jacobi-2d</b> — 中位加速比 <b>1.1668x</b>（基线 1123.63 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb029`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.1658x　最终确认：**1.1668x**　IQR [1.1627, 1.1672]
- 最终 pass 顺序（14 个）：`loop-distribute,loop-rotate,loop-mssa(licm),loop-vectorize,slp-vectorizer,loop-unroll,instcombine,gvn,loop-vectorize,slp-vectorizer,instcombine,simplifycfg,adce,dse`
- 最终 pass 参数：`-slp-threshold=0 -force-target-max-vector-interleave=4 -unroll-threshold=600 -unroll-count=8 -vectorizer-min-trip-count=4`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.6005x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'loop-unroll', 'instcombine', 'gvn', 'loop-distribute', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'adce', 'dse'] params=['-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-unroll-threshold=300', '-unroll-count=4', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.1658x  ACCEPTED (new P*)  passes=['loop-distribute', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'gvn', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'adce', 'dse'] params=['-slp-threshold=0', '-force-target-max-vector-interleave=4', '-unroll-threshold=600', '-unroll-count=8', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=1.1631x  REJECTED, rollback to P*  passes=['loop-distribute', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'gvn', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'adce', 'dse', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine'] params=['-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-unroll-threshold=600', '-unroll-count=8', '-vectorizer-min-trip-count=4'] |

</details>

<details><summary><b>lu</b> — 中位加速比 <b>1.0332x</b>（基线 32506.72 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb020`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0162x　最终确认：**1.0332x**　IQR [1.0098, 1.0346]
- 最终 pass 顺序（18 个）：`loop-mssa(licm),loop-rotate,loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,instcombine,simplifycfg,loop-mssa(licm),loop-unroll,slp-vectorizer,instcombine,simplifycfg,loop-mssa(licm),loop-unroll,loop-vectorize,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=8 -unroll-partial-threshold=300 -force-target-max-vector-interleave=4 -vectorizer-min-trip-count=4 -slp-threshold=-5`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0114x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-slp-threshold=0'] |
| 2/3 | speedup=1.0162x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-5'] |
| 3/3 | speedup=1.0069x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=1200', '-unroll-count=8', '-unroll-partial-threshold=600', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-10'] |

</details>

<details><summary><b>ludcmp</b> — 中位加速比 <b>1.0123x</b>（基线 33249.89 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb019`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0151x　最终确认：**1.0123x**　IQR [1.0075, 1.0144]
- 最终 pass 顺序（17 个）：`loop-rotate,loop-versioning,loop-versioning-licm,loop-mssa(licm),loop-distribute,loop-vectorize,loop-unroll,instcombine,simplifycfg,loop-unroll,instcombine,slp-vectorizer,instcombine,simplifycfg,loop-unroll,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -vectorizer-min-trip-count=4 -slp-threshold=-5 -force-target-max-vector-interleave=4 -licm-max-num-uses-traversed=128 -unroll-partial-threshold=300 -unroll-max-count=4`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0058x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'loop-distribute', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-licm-max-num-uses-traversed=128'] |
| 2/3 | speedup=1.0050x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'loop-distribute', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=800', '-unroll-count=8', '-vectorizer-min-trip-count=4', '-slp-threshold=-10', '-force-target-max-vector-interleave=8', '-licm-max-num-uses-traversed=128'] |
| 3/3 | speedup=1.0151x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-versioning', 'loop-versioning-licm', 'loop-mssa(licm)', 'loop-distribute', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-unroll', 'instcombine', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-licm-max-num-uses-traversed=128', '-unroll-partial-threshold=300', '-unroll-max-count=4'] |

</details>

<details><summary><b>mvt</b> — 中位加速比 <b>0.9903x</b>（基线 37.23 ms，? 校验，4/15 次为正）</summary>

- 任务 `po_pb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**0.9903x**　IQR [0.9835, 1.0084]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8656x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-versioning-licm', 'loop-mssa(licm)', 'loop-idiom', 'indvars', 'loop-simplifycfg', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2'] |
| 2/3 | speedup=0.9334x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-simplifycfg', 'loop-vectorize', 'vector-combine', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=2', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-slp-threshold=-5'] |
| 3/3 | speedup=0.8814x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-versioning-licm', 'loop-mssa(licm)', 'indvars', 'loop-simplifycfg', 'loop-vectorize', 'vector-combine', 'loop-unroll', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2'] |

</details>

<details><summary><b>network_dijkstra</b> — 中位加速比 <b>0.9960x</b>（基线 1.07 ms，? 校验，22/51 次为正）</summary>

- 任务 `po_cb011`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0136x　最终确认：**0.9960x**　IQR [0.8042, 1.3269]
- 最终 pass 顺序（22 个）：`sroa,early-cse,simplifycfg,instcombine,gvn,loop-mssa(licm),loop-rotate,indvars,loop-unroll,loop-unroll,loop-vectorize,slp-vectorizer,gvn,loop-mssa(licm),instcombine,simplifycfg,adce,loop-unroll,slp-vectorizer,gvn,loop-mssa(licm),instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=8 -unroll-partial-threshold=300 -slp-threshold=-10 -vectorizer-min-trip-count=4 -licm-max-num-uses-traversed=128 -gvn-max-num-deps=200 -inline-threshold=800 -inlinehint-threshold=1200`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9956x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'gvn', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'adce', 'loop-unroll', 'slp-vectorizer', 'gvn', 'loop-mssa(licm)', 'instcombine'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-gvn-max-num-deps=200', '-inline-threshold=400',  |
| 2/3 | speedup=1.0136x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'gvn', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'adce', 'loop-unroll', 'slp-vectorizer', 'gvn', 'loop-mssa(licm)', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-gvn-max-num-deps=200', '-inline-threshold=800', '-inlinehint-threshold=1 |
| 3/3 | speedup=0.7697x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'slp-vectorizer', 'gvn', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'adce', 'loop-unroll', 'slp-vectorizer', 'loop-unroll', 'gvn', 'loop-mssa(licm)', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-gvn-max-num-deps= |

</details>

<details><summary><b>network_patricia</b> — 中位加速比 <b>0.9949x</b>（基线 1.62 ms，? 校验，20/51 次为正）</summary>

- 任务 `po_cb012`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0026x　最终确认：**0.9949x**　IQR [0.9813, 1.0124]
- 最终 pass 顺序（24 个）：`sroa,early-cse,instcombine,simplifycfg,gvn,gvn-hoist,gvn-sink,loop-mssa(licm),loop-rotate,indvars,loop-unroll,instcombine,simplifycfg,jump-threading,correlated-propagation,sccp,adce,bdce,dse,reassociate,instsimplify,simplifycfg,loop-mssa(licm),loop-unroll`
- 最终 pass 参数：`-unroll-threshold=150 -unroll-count=2 -unroll-partial-threshold=50 -unroll-max-count=4 -inline-threshold=225 -inlinehint-threshold=325 -slp-threshold=0 -force-target-max-vector-interleave=1 -vectorizer-min-trip-count=8 -licm-max-num-uses-traversed=32 -jump-threading-threshold=6 -gvn-max-num-deps=100 -loop-distribute-scev-check-threshold=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9880x  REJECTED, rollback to P*  passes=['early-cse', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'gvn-sink', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'sroa', 'instcombine', 'simplifycfg', 'adce', 'dse', 'memcpyopt', 'mldst-motion', 'slsr', 'reassociate', 'jump-threading', 'correlated-propagation', 'sccp', 'bdce', 'dce', 'loop-unroll'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8', '-inline-threshold=400', '-inlinehint-threshold=600', '-slp-threshold=-5', '-force-target-max-vector-interleave=2', '-v |
| 2/3 | speedup=0.9979x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'gvn', 'aggressive-instcombine', 'simplifycfg', 'adce', 'bdce', 'dse', 'mldst-motion', 'reassociate', 'jump-threading', 'correlated-propagation', 'sccp', 'dce', 'loop-unroll'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-unroll-max-count=16', '-inline-threshold=800', '-inlinehint-threshold=1200', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '-vectorizer-mi |
| 3/3 | speedup=1.0026x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'gvn-sink', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'instcombine', 'simplifycfg', 'jump-threading', 'correlated-propagation', 'sccp', 'adce', 'bdce', 'dse', 'reassociate', 'instsimplify', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=50', '-unroll-max-count=4', '-inline-threshold=225', '-inlinehint-threshold=325', '-slp-threshold=0', '-force-target-max-vector-interleave=1', '-vect |

</details>

<details><summary><b>nussinov</b> — 中位加速比 <b>1.0000x</b>（基线 3715.31 ms，? 校验，None/None 次为正）</summary>

- 任务 `po_pb024`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | FAILED: incorrect: output hash mismatch (ref=0537e816e19a, opt=b5dc2f9c40c7) |
| 2/3 | FAILED: incorrect: output hash mismatch (ref=0537e816e19a, opt=b5dc2f9c40c7) |
| 3/3 | FAILED: incorrect: output hash mismatch (ref=0537e816e19a, opt=b5dc2f9c40c7) |

</details>

<details><summary><b>office_stringsearch2</b> — 中位加速比 <b>1.0009x</b>（基线 2.35 ms，? 校验，28/51 次为正）</summary>

- 任务 `po_cb013`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0000x　最终确认：**1.0009x**　IQR [0.9899, 1.0139]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8475x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-simplifycfg', 'indvars', 'loop-idiom', 'loop-idiom-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'simplifycfg', 'dse', 'mldst-motion', 'gvn', 'sroa', 'slp-vectorizer', 'loop-reduce', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=0', '-force-target-max-vector-interleave=2'] |
| 2/3 | speedup=0.9552x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-simplifycfg', 'indvars', 'loop-idiom', 'loop-idiom-vectorize', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'instcombine', 'simplifycfg', 'dse', 'mldst-motion', 'gvn', 'sroa', 'slp-vectorizer', 'loop-reduce', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-vectorizer-min-trip-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4'] |
| 3/3 | speedup=0.7643x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'loop-simplifycfg', 'indvars', 'loop-idiom', 'loop-idiom-vectorize', 'loop-vectorize', 'loop-unroll', 'instcombine', 'gvn', 'mldst-motion', 'dse', 'slp-vectorizer', 'loop-reduce', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=2', '-vectorizer-min-trip-count=8', '-slp-threshold=-10', '-force-target-max-vector-interleave=2'] |

</details>

<details><summary><b>security_blowfish_decode</b> — 中位加速比 <b>1.0027x</b>（基线 2.28 ms，? 校验，27/51 次为正）</summary>

- 任务 `po_cb021`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0763x　最终确认：**1.0027x**　IQR [0.9807, 1.0185]
- 最终 pass 顺序（20 个）：`sroa,early-cse,simplifycfg,instcombine,gvn,jump-threading,correlated-propagation,dse,loop-rotate,loop-unroll,loop-unroll-full,indvars,loop-simplifycfg,loop-mssa(licm),loop-sink,slp-vectorizer,instcombine,simplifycfg,dce,bdce`
- 最终 pass 参数：`-unroll-threshold=300 -unroll-count=4 -unroll-partial-threshold=150 -slp-threshold=-5 -inline-threshold=400`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0763x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'jump-threading', 'correlated-propagation', 'dse', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'indvars', 'loop-simplifycfg', 'loop-mssa(licm)', 'loop-sink', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'dce', 'bdce'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-slp-threshold=-5', '-inline-threshold=400'] |
| 2/3 | speedup=1.0680x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'gvn-hoist', 'correlated-propagation', 'jump-threading', 'dse', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'indvars', 'loop-simplifycfg', 'loop-mssa(licm)', 'loop-sink', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'tailcallelim', 'mergereturn', 'dce', 'bdce'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-inline-threshold=800'] |
| 3/3 | speedup=0.9959x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'jump-threading', 'correlated-propagation', 'dse', 'loop-rotate', 'loop-unroll', 'loop-unroll-full', 'indvars', 'loop-simplifycfg', 'loop-mssa(licm)', 'loop-sink', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'tailcallelim', 'mergereturn', 'dce', 'bdce'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-slp-threshold=-5', '-inline-threshold=400'] |

</details>

<details><summary><b>security_blowfish_encode</b> — 中位加速比 <b>1.0011x</b>（基线 2.13 ms，? 校验，27/51 次为正）</summary>

- 任务 `po_cb020`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0817x　最终确认：**1.0011x**　IQR [0.9928, 1.0096]
- 最终 pass 顺序（14 个）：`early-cse,gvn,instcombine,simplifycfg,loop-rotate,loop-unroll,loop-mssa(licm),gvn,instcombine,simplifycfg,loop-unroll,gvn,instcombine,simplifycfg`
- 最终 pass 参数：`-inline-threshold=800 -inlinehint-threshold=1200 -unroll-threshold=600 -unroll-count=8 -unroll-partial-threshold=300 -unroll-max-count=16 -slp-threshold=-5`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0247x  ACCEPTED (new P*)  passes=['early-cse', 'gvn', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg'] params=['-inline-threshold=400', '-inlinehint-threshold=600', '-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8'] |
| 2/3 | speedup=1.0817x  ACCEPTED (new P*)  passes=['early-cse', 'gvn', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'gvn', 'instcombine', 'simplifycfg'] params=['-inline-threshold=800', '-inlinehint-threshold=1200', '-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-unroll-max-count=16', '-slp-threshold=-5'] |
| 3/3 | speedup=1.0125x  REJECTED, rollback to P*  passes=['early-cse', 'gvn', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll-full', 'gvn', 'instcombine', 'simplifycfg', 'sccp', 'adce', 'instcombine', 'simplifycfg'] params=['-inline-threshold=1200', '-inlinehint-threshold=1600', '-unroll-threshold=800', '-unroll-count=8', '-unroll-partial-threshold=300', '-unroll-max-count=16', '-slp-threshold=-5'] |

</details>

<details><summary><b>security_rijndael_decode</b> — 中位加速比 <b>1.0056x</b>（基线 2.16 ms，? 校验，30/51 次为正）</summary>

- 任务 `po_cb014`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0518x　最终确认：**1.0056x**　IQR [0.9801, 1.0233]
- 最终 pass 顺序（18 个）：`simplifycfg,instcombine,sroa,early-cse,jump-threading,correlated-propagation,gvn,loop-mssa(licm),loop-rotate,loop-unroll,loop-vectorize,slp-vectorizer,instcombine,simplifycfg,gvn-hoist,gvn-sink,adce,instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -slp-threshold=-10 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0388x  ACCEPTED (new P*)  passes=['simplifycfg', 'instcombine', 'sroa', 'early-cse', 'jump-threading', 'correlated-propagation', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.0518x  ACCEPTED (new P*)  passes=['simplifycfg', 'instcombine', 'sroa', 'early-cse', 'jump-threading', 'correlated-propagation', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn-hoist', 'gvn-sink', 'adce', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-10', '-vectorizer-min-trip-count=8'] |
| 3/3 | speedup=1.0513x  REJECTED, rollback to P*  passes=['simplifycfg', 'instcombine', 'sroa', 'early-cse', 'jump-threading', 'correlated-propagation', 'gvn', 'loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn-hoist', 'gvn-sink', 'adce', 'instcombine', 'loop-unroll', 'slp-vectorizer', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=8', '-slp-threshold=-10', '-vectorizer-min-trip-count=8', '-jump-threading-threshold=6', '-licm-max-num-uses-traversed=32'] |

</details>

<details><summary><b>security_rijndael_encode</b> — 中位加速比 <b>1.0010x</b>（基线 2.22 ms，? 校验，27/51 次为正）</summary>

- 任务 `po_cb015`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0230x　最终确认：**1.0010x**　IQR [0.9538, 1.0205]
- 最终 pass 顺序（18 个）：`loop-mssa(licm),loop-rotate,instcombine,simplifycfg,loop-simplifycfg,loop-unroll,instcombine,simplifycfg,loop-vectorize,slp-vectorizer,instcombine,simplifycfg,gvn,gvn-hoist,dce,loop-unroll,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -slp-threshold=-5 -vectorizer-min-trip-count=4 -force-target-max-vector-interleave=4 -jump-threading-threshold=6`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0181x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-rotate', 'instcombine', 'simplifycfg', 'loop-simplifycfg', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'dce'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=0', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2'] |
| 2/3 | speedup=1.0230x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-rotate', 'instcombine', 'simplifycfg', 'loop-simplifycfg', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'dce', 'loop-unroll', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-jump-threading-threshold=6'] |
| 3/3 | speedup=1.0027x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'instcombine', 'simplifycfg', 'loop-simplifycfg', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'simplifycfg', 'gvn', 'gvn-hoist', 'dce', 'loop-unroll', 'instcombine', 'simplifycfg', 'loop-unroll-full', 'instcombine', 'simplifycfg', 'dce'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-jump-threading-threshold=6'] |

</details>

<details><summary><b>security_sha</b> — 中位加速比 <b>0.9966x</b>（基线 1.87 ms，? 校验，24/51 次为正）</summary>

- 任务 `po_cb016`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.2106x　最终确认：**0.9966x**　IQR [0.9760, 1.0921]
- 最终 pass 顺序（12 个）：`loop-rotate,loop-mssa(licm),loop-unroll-full,loop-mssa(licm),gvn,instcombine,loop-rotate,loop-vectorize,loop-unroll,instcombine,gvn,simplifycfg`
- 最终 pass 参数：`-unroll-count=4 -unroll-threshold=300 -vectorizer-min-trip-count=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.2106x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll-full', 'loop-mssa(licm)', 'gvn', 'instcombine', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'gvn', 'simplifycfg'] params=['-unroll-count=4', '-unroll-threshold=300', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=1.0077x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll-full', 'loop-mssa(licm)', 'gvn', 'instcombine', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'gvn', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll-full', 'loop-mssa(licm)', 'gvn', 'instcombine'] params=['-unroll-count=8', '-unroll-threshold=600', '-vectorizer-min-trip-count=4', '-slp-threshold=-5'] |
| 3/3 | speedup=1.0176x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll-full', 'loop-mssa(licm)', 'gvn', 'instcombine', 'loop-rotate', 'loop-vectorize', 'loop-unroll', 'instcombine', 'gvn', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'loop-unroll', 'instcombine', 'gvn', 'simplifycfg'] params=['-unroll-count=4', '-unroll-threshold=300', '-vectorizer-min-trip-count=8', '-slp-threshold=0'] |

</details>

<details><summary><b>seidel-2d</b> — 中位加速比 <b>1.0000x</b>（基线 37870.23 ms，? 校验，0/3 次为正，⚠ 正确性门无效、孤儿抢核、PO 预算被 InstCombine 吞）</summary>

- 任务 `po_pb030`，节点 `dgx-spark-a-0`，数据源 `arch`
- 探索期最好单次：1.0000x　最终确认：**1.0000x**　IQR [0.9999, 1.0000]
- 最终 pass 顺序（1 个）：`default<O3>`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.8668x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'gvn'] params=['-unroll-threshold=300', '-unroll-count=4', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=8'] |
| 2/3 | speedup=0.8594x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'gvn', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-10', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=0.9011x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'loop-unroll', 'slp-vectorizer', 'loop-mssa(licm)', 'loop-vectorize', 'slp-vectorizer', 'gvn', 'instcombine', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer'] params=['-unroll-threshold=150', '-unroll-count=2', '-slp-threshold=-5', '-force-target-max-vector-interleave=2', '-vectorizer-min-trip-count=4'] |

</details>

<details><summary><b>symm</b> — 中位加速比 <b>1.1677x</b>（基线 4315.11 ms，? 校验，2/3 次为正）</summary>

- 任务 `po_pb006`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0560x　最终确认：**1.1677x**　IQR [0.9075, 1.2278]
- 最终 pass 顺序（21 个）：`sroa,early-cse,instcombine,loop-rotate,loop-mssa(licm),loop-unroll,loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,loop-mssa(licm),gvn,instcombine,simplifycfg,slp-vectorizer,slp-vectorizer,loop-unroll,loop-vectorize,instcombine,gvn,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=300 -unroll-count=4 -unroll-partial-threshold=300 -unroll-max-count=8 -slp-threshold=-5 -force-target-max-vector-interleave=4 -vectorizer-min-trip-count=4 -licm-max-num-uses-traversed=128 -gvn-max-num-deps=200`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9315x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'slp-vectorizer', 'slp-vectorizer', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'loop-unroll', 'slp-vectorizer', 'loop-vectorize', 'instcombine', 'gvn', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-gvn-max-num-de |
| 2/3 | speedup=1.0560x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'slp-vectorizer', 'loop-unroll', 'loop-vectorize', 'instcombine', 'gvn', 'simplifycfg'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses-traversed=128', '-gvn |
| 3/3 | speedup=1.0332x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'instcombine', 'loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'gvn', 'instcombine', 'simplifycfg', 'slp-vectorizer', 'slp-vectorizer', 'loop-unroll', 'loop-vectorize', 'instcombine', 'gvn', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-slp-threshold=-5', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4', '-licm-max-num-uses |

</details>

<details><summary><b>syr2k</b> — 中位加速比 <b>1.0040x</b>（基线 2916.90 ms，? 校验，2/3 次为正）</summary>

- 任务 `po_pb007`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0151x　最终确认：**1.0040x**　IQR [0.9976, 1.0304]
- 最终 pass 顺序（15 个）：`loop-rotate,loop-mssa(licm),loop-unroll,loop-unroll,loop-simplifycfg,indvars,loop-versioning-licm,loop-mssa(licm),loop-vectorize,loop-unroll,instcombine,simplifycfg,gvn,dse,adce`
- 最终 pass 参数：`-unroll-threshold=150 -unroll-count=8 -unroll-partial-threshold=150 -licm-max-num-uses-traversed=128 -force-target-max-vector-interleave=8 -vectorizer-min-trip-count=4 -jump-threading-threshold=12`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9942x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-unroll-full', 'loop-simplifycfg', 'indvars', 'loop-versioning-licm', 'loop-unroll', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-licm-max-num-uses-traversed=128', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |
| 2/3 | speedup=0.9967x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-unroll', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-mssa(licm)', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-licm-max-num-uses-traversed=128', '-force-target-max-vector-interleave=4', '-vectorizer-min-trip-count=4'] |
| 3/3 | speedup=1.0151x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'loop-unroll', 'loop-unroll', 'loop-simplifycfg', 'indvars', 'loop-versioning-licm', 'loop-mssa(licm)', 'loop-vectorize', 'loop-unroll', 'instcombine', 'simplifycfg', 'gvn', 'dse', 'adce'] params=['-unroll-threshold=150', '-unroll-count=8', '-unroll-partial-threshold=150', '-licm-max-num-uses-traversed=128', '-force-target-max-vector-interleave=8', '-vectorizer-min-trip-count=4', '-jump-threading-threshold=12'] |

</details>

<details><summary><b>syrk</b> — 中位加速比 <b>1.1970x</b>（基线 1189.39 ms，? 校验，3/3 次为正）</summary>

- 任务 `po_pb008`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0688x　最终确认：**1.1970x**　IQR [1.1942, 1.2675]
- 最终 pass 顺序（20 个）：`loop-mssa(licm),loop-rotate,indvars,loop-unroll,loop-vectorize,loop-mssa(licm),loop-unroll,loop-vectorize,slp-vectorizer,instcombine,gvn,loop-distribute,loop-versioning-licm,loop-mssa(licm),instcombine,gvn,loop-unroll,loop-vectorize,slp-vectorizer,instcombine`
- 最终 pass 参数：`-unroll-threshold=450 -unroll-count=4 -unroll-partial-threshold=200 -unroll-max-count=8 -vectorizer-min-trip-count=4 -force-target-max-vector-interleave=2 -licm-max-num-uses-traversed=128 -slp-threshold=-5 -gvn-max-num-deps=100`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9610x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-distribute', 'loop-versioning-licm', 'loop-mssa(licm)', 'instcombine', 'gvn', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine'] params=['-unroll-threshold=300', '-unroll-count=4', '-unroll-partial-threshold=150', '-unroll-max-count=8', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=2', '-licm-max-num-uses-traversed=128', '-slp-threshold |
| 2/3 | speedup=0.9350x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'loop-distribute', 'loop-versioning-licm', 'loop-mssa(licm)', 'instcombine', 'gvn', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=16', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4', '-licm-max-num-uses-traversed=128', '-slp-threshol |
| 3/3 | speedup=1.0688x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-vectorize', 'loop-mssa(licm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine', 'gvn', 'loop-distribute', 'loop-versioning-licm', 'loop-mssa(licm)', 'instcombine', 'gvn', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'instcombine'] params=['-unroll-threshold=450', '-unroll-count=4', '-unroll-partial-threshold=200', '-unroll-max-count=8', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=2', '-licm-max-num-uses-traversed=128', '-slp-threshold=-5' |

</details>

<details><summary><b>telecom_adpcm_c</b> — 中位加速比 <b>0.9973x</b>（基线 2.68 ms，? 校验，24/51 次为正）</summary>

- 任务 `po_cb017`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0369x　最终确认：**0.9973x**　IQR [0.9676, 1.0180]
- 最终 pass 顺序（27 个）：`loop-rotate,indvars,loop-mssa(licm),loop-mssa(lnicm),nary-reassociate,slsr,loop-reduce,loop-unroll,loop-unroll-full,loop-vectorize,slp-vectorizer,vector-combine,loop-simplifycfg,simplifycfg,instcombine,gvn,loop-rotate,indvars,loop-mssa(licm),loop-mssa(lnicm),loop-unroll,loop-vectorize,slp-vectorizer,vector-combine,simplifycfg,instcombine,gvn`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=8 -slp-threshold=-5 -vectorizer-min-trip-count=4 -force-target-max-vector-interleave=2`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0269x  ACCEPTED (new P*)  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'nary-reassociate', 'slsr', 'loop-reduce', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'loop-simplifycfg', 'simplifycfg', 'instcombine', 'gvn'] params=['-unroll-threshold=600', '-unroll-count=4', '-slp-threshold=-5', '-vectorizer-min-trip-count=4'] |
| 2/3 | speedup=1.0369x  ACCEPTED (new P*)  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'nary-reassociate', 'slsr', 'loop-reduce', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'loop-simplifycfg', 'simplifycfg', 'instcombine', 'gvn', 'loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'simplifycfg', 'instcombine', 'gvn'] params=['-unroll-threshold=600', '-unroll-count=8', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-force-target-max-vector- |
| 3/3 | speedup=0.9376x  REJECTED, rollback to P*  passes=['loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'nary-reassociate', 'slsr', 'loop-reduce', 'loop-unroll', 'loop-unroll-full', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'loop-simplifycfg', 'simplifycfg', 'instcombine', 'gvn', 'loop-rotate', 'indvars', 'loop-mssa(licm)', 'loop-mssa(lnicm)', 'loop-unroll', 'loop-vectorize', 'slp-vectorizer', 'vector-combine', 'simplifycfg', 'instcombine', 'gvn'] params=['-unroll-threshold=600', '-unroll-count=8', '-slp-threshold=-10', '-vectorizer-min-trip-count=4', '-force-target-max |

</details>

<details><summary><b>telecom_adpcm_d</b> — 中位加速比 <b>0.9966x</b>（基线 2.43 ms，? 校验，20/51 次为正）</summary>

- 任务 `po_cb018`，节点 `dgx-spark-a-0`，数据源 `live`
- 探索期最好单次：1.0193x　最终确认：**0.9966x**　IQR [0.9825, 1.0072]
- 最终 pass 顺序（18 个）：`simplifycfg,loop-simplifycfg,break-crit-edges,loop-rotate,loop-mssa(licm),indvars,loop-unroll,loop-deletion,gvn,gvn-hoist,sccp,jump-threading,simplifycfg,instcombine,slp-vectorizer,loop-vectorize,loop-unroll,instcombine`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -slp-threshold=-5 -vectorizer-min-trip-count=4 -jump-threading-threshold=12 -gvn-max-num-deps=100 -loop-distribute-scev-check-threshold=8`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | pre-flight dropped 1 rejected item(s): |
| 1/3 | speedup=1.0193x  ACCEPTED (new P*)  passes=['simplifycfg', 'loop-simplifycfg', 'break-crit-edges', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-deletion', 'gvn', 'gvn-hoist', 'sccp', 'jump-threading', 'simplifycfg', 'instcombine', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-jump-threading-threshold=12', '-gvn-max-num-deps=100', '-loop-distribute-scev-check-threshold=8'] |
| 2/3 | speedup=0.9887x  REJECTED, rollback to P*  passes=['simplifycfg', 'loop-simplifycfg', 'break-crit-edges', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-deletion', 'gvn', 'gvn-hoist', 'sccp', 'jump-threading', 'simplifycfg', 'instcombine', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine', 'loop-versioning-licm', 'loop-unroll'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-jump-threading-threshold=12', '-gvn-max-num-deps=100', '-loop-distribute-scev-check-threshold=8' |
| 3/3 | speedup=1.0086x  REJECTED, rollback to P*  passes=['simplifycfg', 'loop-simplifycfg', 'break-crit-edges', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'loop-deletion', 'gvn', 'gvn-hoist', 'sccp', 'jump-threading', 'simplifycfg', 'instcombine', 'slp-vectorizer', 'loop-vectorize', 'loop-unroll', 'instcombine', 'loop-unroll-full', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-slp-threshold=-5', '-vectorizer-min-trip-count=4', '-jump-threading-threshold=12', '-gvn-max-num-deps=100', '-loop-distribute-scev-check-threshold=8'] |

</details>

<details><summary><b>telecom_crc32</b> — 中位加速比 <b>1.0019x</b>（基线 1.89 ms，? 校验，27/51 次为正）</summary>

- 任务 `po_cb019`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0261x　最终确认：**1.0019x**　IQR [0.9924, 1.0205]
- 最终 pass 顺序（20 个）：`sroa,early-cse,simplifycfg,instcombine,gvn,loop-rotate,loop-mssa(licm),indvars,loop-unroll,instcombine,simplifycfg,gvn,dce,adce,loop-unroll,loop-unroll,instcombine,simplifycfg,dce,adce`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=8 -unroll-partial-threshold=300 -inline-threshold=800 -inlinehint-threshold=600 -jump-threading-threshold=12`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0187x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'instcombine', 'simplifycfg', 'gvn', 'dce', 'adce'] params=['-unroll-threshold=300', '-unroll-count=4', '-inline-threshold=400', '-inlinehint-threshold=600'] |
| 2/3 | speedup=1.0261x  ACCEPTED (new P*)  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'instcombine', 'simplifycfg', 'gvn', 'dce', 'adce', 'loop-unroll', 'loop-unroll', 'instcombine', 'simplifycfg', 'dce', 'adce'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=300', '-inline-threshold=800', '-inlinehint-threshold=600', '-jump-threading-threshold=12'] |
| 3/3 | speedup=1.0004x  REJECTED, rollback to P*  passes=['sroa', 'early-cse', 'simplifycfg', 'instcombine', 'gvn', 'loop-rotate', 'loop-mssa(licm)', 'indvars', 'loop-unroll', 'instcombine', 'simplifycfg', 'gvn', 'dce', 'adce', 'loop-unroll', 'loop-unroll', 'instcombine', 'simplifycfg', 'dce', 'adce', 'jump-threading', 'sccp', 'adce', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=8', '-unroll-partial-threshold=600', '-inline-threshold=1600', '-inlinehint-threshold=1200', '-jump-threading-threshold=24'] |

</details>

<details><summary><b>trisolv</b> — 中位加速比 <b>1.0043x</b>（基线 13.33 ms，? 校验，23/43 次为正）</summary>

- 任务 `po_pb021`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0786x　最终确认：**1.0043x**　IQR [0.9807, 1.0445]
- 最终 pass 顺序（19 个）：`loop-rotate,loop-mssa(licm),indvars,gvn,dse,adce,loop-unroll,loop-mssa(licm),gvn,dse,instcombine,simplifycfg,loop-rotate,indvars,loop-unroll,gvn,dse,instcombine,simplifycfg`
- 最终 pass 参数：`-unroll-threshold=150 -unroll-count=2 -unroll-partial-threshold=150 -unroll-max-count=4 -licm-max-num-uses-traversed=32`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=0.9722x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'dse', 'dce', 'adce', 'instcombine', 'simplifycfg', 'loop-mssa(licm)', 'loop-unroll', 'gvn', 'dse', 'dce', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-unroll-max-count=8', '-licm-max-num-uses-traversed=128'] |
| 2/3 | speedup=1.0786x  ACCEPTED (new P*)  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'gvn', 'dse', 'adce', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'dse', 'instcombine', 'simplifycfg', 'loop-rotate', 'indvars', 'loop-unroll', 'gvn', 'dse', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=150', '-unroll-max-count=4', '-licm-max-num-uses-traversed=32'] |
| 3/3 | speedup=0.9170x  REJECTED, rollback to P*  passes=['loop-rotate', 'loop-mssa(licm)', 'indvars', 'gvn', 'dse', 'adce', 'loop-unroll', 'loop-mssa(licm)', 'gvn', 'dse', 'instcombine', 'simplifycfg', 'loop-rotate', 'indvars', 'loop-unroll', 'gvn', 'dse', 'instcombine', 'simplifycfg', 'loop-rotate', 'loop-mssa(licm)', 'gvn', 'dse', 'adce', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=150', '-unroll-count=2', '-unroll-partial-threshold=150', '-unroll-max-count=4', '-licm-max-num-uses-traversed=32'] |

</details>

<details><summary><b>trmm</b> — 中位加速比 <b>0.9981x</b>（基线 2188.69 ms，? 校验，1/3 次为正）</summary>

- 任务 `po_pb009`，节点 `dgx-spark-b-0`，数据源 `live`
- 探索期最好单次：1.0159x　最终确认：**0.9981x**　IQR [0.9933, 1.0027]
- 最终 pass 顺序（31 个）：`loop-mssa(licm),loop-mssa(licm),loop-rotate,indvars,loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,loop-mssa(licm),loop-mssa(licm),gvn,gvn,instcombine,instcombine,simplifycfg,sroa,instcombine,simplifycfg,loop-unroll,loop-unroll,loop-vectorize,loop-vectorize,loop-mssa(licm),loop-mssa(licm),gvn,instcombine,loop-data-prefetch,loop-unroll,loop-unroll,loop-vectorize,loop-vectorize`
- 最终 pass 参数：`-unroll-threshold=600 -unroll-count=4 -unroll-partial-threshold=300 -vectorizer-min-trip-count=8 -force-target-max-vector-interleave=4 -licm-max-num-uses-traversed=128`

每轮候选（AutoPass R3：每轮 Reasoning Agent 产出一条 pass 顺序 + 参数，Evaluation Agent 严格 `t(P) < t(P*)` 才接受）：

| 轮 | 结果 |
|---:|---|
| 1/3 | speedup=1.0097x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-unroll', 'loop-unroll', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'gvn', 'instcombine', 'instcombine', 'simplifycfg', 'sroa', 'instcombine', 'simplifycfg'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-vectorizer-min-trip-count=8', '-force-target-max-vector-interleave=4', '-licm-max-num-uses-traversed=128'] |
| 2/3 | speedup=0.9976x  REJECTED, rollback to P*  passes=['loop-mssa(licm)', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'gvn', 'instcombine', 'instcombine', 'simplifycfg', 'sroa', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'instcombine'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshold=300', '-vectorizer-min-trip-count=4', '-force-target-max-vector-interleave=4',  |
| 3/3 | speedup=1.0159x  ACCEPTED (new P*)  passes=['loop-mssa(licm)', 'loop-mssa(licm)', 'loop-rotate', 'indvars', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'gvn', 'instcombine', 'instcombine', 'simplifycfg', 'sroa', 'instcombine', 'simplifycfg', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize', 'loop-mssa(licm)', 'loop-mssa(licm)', 'gvn', 'instcombine', 'loop-data-prefetch', 'loop-unroll', 'loop-unroll', 'loop-vectorize', 'loop-vectorize'] params=['-unroll-threshold=600', '-unroll-count=4', '-unroll-partial-threshol |

</details>

## 4. 被拒候选分析

agent 提出但未被采纳的候选，按拒绝原因归类。这些不是 bug，是方法本身的一部分：确认门拦掉了探索期的虚高读数与不正确的改写。

| 拒绝原因 | 次数 |
|---|---:|
| pragma 未匹配到循环 | 97 |
| 编译失败 | 70 |
| 其它 | 41 |
| 数值不符（多为浮点重结合） | 15 |
| 哈希不符 | 12 |

**其它** 样例：

- `c1` automotive_qsort1: 步骤3: 失败 [rewrite_source] [SMALL_DATASET] optimized run timed out
- `c1` automotive_susan_smoothing: 步骤2: 失败 [rewrite_source] optimized version returned non-zero exit code -11
- `c1` consumer_tiff2rgba: 步骤2: 失败 [rewrite_source] [SMALL_DATASET] optimized version returned non-zero exit code -6
- `c1` consumer_tiff2rgba: 步骤9: 失败 [rewrite_source] [SMALL_DATASET] optimized version returned non-zero exit code -11

**编译失败** 样例：

- `c1` automotive_qsort1: 步骤6: 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp60fxk02p/rw_utils_utils_shadow/polybench.c:665:25: error: use of undeclare
- `c1` automotive_qsort1: 步骤7: 失败 [rewrite_source] 候选编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmppo4qzxld/rw_utils_utils_shadow/polybench.c:661:25: error: use of undeclare
- `c1` automotive_susan_corners: 步骤1: 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmpzj0m3ovt/automotive_susan_corners_rewrit
- `c1` automotive_susan_corners: 步骤2: 失败 [rewrite_source] compile error (fix also failed): 优化版编译失败 (SMALL_DATASET): /home/hanning/comet/tmp/tmp1q3j3orz/automotive_susan_corners_rewrit

**哈希不符** 样例：

- `c1` automotive_susan_smoothing: 步骤1: 失败 [rewrite_source] precision error (fix also failed): output hash mismatch (ref=0e44d5392b3e, opt=400ab85810c4)
- `c1` telecom_adpcm_c: 步骤5: 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=6227febad457, opt=6025e67c9bba)
- `c1` telecom_adpcm_d: 步骤3: 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=f14432f8dd7b, opt=f89573bb1b00)
- `c1` telecom_adpcm_d: 步骤5: 失败 [rewrite_source] [SMALL_DATASET] output hash mismatch (ref=f14432f8dd7b, opt=f89573bb1b00)

**pragma 未匹配到循环** 样例：

- `c2` automotive_qsort1: 步骤4: 失败 [try_pragma] 未找到匹配的 for 循环前缀
- `c2` automotive_susan_corners: 步骤3: 失败 [try_pragma] 未找到匹配的 for 循环前缀
- `c2` automotive_susan_corners: 步骤6: 失败 [try_pragma] 未找到匹配的 for 循环前缀
- `c2` automotive_susan_corners: 步骤9: 失败 [try_pragma] 未找到匹配的 for 循环前缀

**数值不符（多为浮点重结合）** 样例：

- `c3` gesummv: 步骤4: 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.68e-04 at index 74 (ref=59.48, opt=59.47), epsilon=1.00e-04
- `c3` gesummv: 步骤7: 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 1.68e-04 at index 74 (ref=59.48, opt=59.47), epsilon=1.00e-04
- `c3` syr2k: 步骤4: 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 5.59e-03 at index 3840 (ref=1.78, opt=1.79), epsilon=1.00e-04
- `c3` syr2k: 步骤7: 失败 [try_pragma] [SMALL_DATASET] Numeric mismatch: max relative error 5.59e-03 at index 3840 (ref=1.78, opt=1.79), epsilon=1.00e-04

## 5. 数据来源与复算

- 队列：`oracle4:/home/hanning/comet_queue/state.json`（当前）+ `/home/hanning/comet_queue/state_archive_20260802_prefix.json`（2026-08-02 之前）
- 日志：各节点 `/home/hanning/comet/logs_queue_run_v2` 与 `/home/hanning/comet/logs_queue_run_v2_archive_20260802_prefix`，按队列记录的 `node` 字段定位
- 加速比取 `confirmed_median`；2026-08-02 之前的数据 `final_speedup` 存的是 max-of-n，本文件不使用该字段
- 重跑任务使用新 id，每 (条件, 程序) 只取最近一次完成
