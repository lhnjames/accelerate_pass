# PolyBench 优化实验报告

**时间**：2026-06-23 19:03 — 2026-06-24 11:37（共 ~16.5 小时）  
**配置**：30 个 benchmark，每个 9 rounds（1 param + 8 source），3 并发  
**Geomean 最终 speedup**：3.187x  

---

## 总览

| 分类 | Benchmark | Baseline (ms) | Param | Source | 最终 Best |
|------|-----------|--------------|-------|--------|----------|
| **大幅提升 >5x** | gramschmidt | 10964 | 1.022x | **16.727x** | 16.727x |
| | correlation | 5851 | 1.001x | **16.454x** | 16.454x |
| | trmm | 2521 | 1.014x | **13.032x** | 13.032x |
| | nussinov | 4872 | 1.028x | **12.533x** | 12.533x |
| | doitgen | 588 | 1.047x | **8.110x** | 8.110x |
| | syrk | 1624 | 1.035x | **7.853x** | 7.853x |
| | 2mm | 2143 | 1.030x | **7.358x** | 7.358x |
| | 3mm | 3494 | 1.100x | **7.299x** | 7.299x |
| | symm | 2971 | 1.116x | **6.093x** | 6.093x |
| | syr2k | 4719 | 1.063x | **5.537x** | 5.537x |
| **中等提升 2-5x** | ludcmp | 5347 | 1.126x | **4.525x** | 4.525x |
| | mvt | 5.45 | — | **4.143x** | 4.143x |
| | durbin | 3.15 | 1.100x | **3.894x** | 3.894x |
| | bicg | 10.64 | 1.031x | **3.713x** | 3.713x |
| | gemver | 26.62 | 1.021x | **3.499x** | 3.499x |
| | deriche | 311 | 1.122x | **2.221x** | 2.221x |
| | atax | 7.00 | 1.083x | **2.026x** | 2.026x |
| **小幅提升 <2x** | fdtd-2d | 2253 | **1.393x** | 1.850x | 1.850x |
| | trisolv | 3.69 | **1.202x** | 1.818x | 1.818x |
| | gesummv | 4.78 | 1.055x | 1.686x | 1.686x |
| | gemm | 423 | 1.031x | 1.352x | 1.352x |
| | floyd-warshall | 16061 | **1.094x** | 1.182x | 1.182x |
| **Param 为主** | heat-3d | 2109 | **1.281x** | 1.023x | 1.281x |
| | lu | 5726 | **1.151x** | ✗ 全部失败 | 1.151x |
| | jacobi-2d | 1653 | **1.114x** | 1.002x | 1.114x |
| | covariance | 6293 | **1.091x** | ✗ 全部失败 | 1.091x |
| | jacobi-1d | 0.47 | 1.071x | 1.040x | 1.071x |
| | cholesky | 1449 | 1.012x | ✗ 全部失败 | 1.012x |
| | adi | 9855 | 1.016x | 1.002x | 1.016x |
| | seidel-2d | 20639 | 1.027x | 1.017x | 1.027x |

---

## 逐 Benchmark 分析

### 2mm
- **Param**：LLM 识别出 SLP 向量化被 cost threshold 阻断 + LICM 别名分析不足，选 `-instcombine-max-iterations=6`，得 1.030x；独立测试 retune 发现同 flag=1 反而更好（1.066x）。
- **Source**：8 轮全部通过，从 round 1 的 5.852x 稳步迭代到 round 8 的 **7.358x**。LLM 分析 B/C 矩阵非单位 stride 导致 cache miss，改写为行优先访问 + loop tiling，编译器随后能有效向量化。
- **问题**：无明显问题，迭代稳定。

### 3mm
- **Param**：识别出向量化被 cost model 拒绝，选 `-loop-distribute-scev-check-threshold=64 -instcombine-max-iterations=3`，得 1.100x。
- **Source**：round 7 出现 empty output（binary crash，可能访问越界），其余轮正常。最终 **7.299x**。LLM 分析与 2mm 类似，内存访问模式重构。
- **问题**：round 7 crash 说明 LLM 偶尔生成边界条件错误的代码，但整体影响不大。

### adi
- **Param**：识别列扫描 stride-N 访问问题，选 `-slp-look-ahead-users-budget=16`，仅 1.016x。
- **Source**：8 轮中 1 轮 compile_error，多轮显著退化（0.644x~0.997x），最终只有 round 7 勉强 1.002x。**基本失败**。
- **问题**：ADI（交替方向隐式法）的列方向 sweep 有严格的依赖顺序，LLM 反复尝试循环重排但要么出错要么更慢。LLM 理解了 bottleneck（stride-N cache miss）但无法在保持正确性的前提下解决它。

### atax
- **Param**：LLM 指出双遍历 A 矩阵 + y 的 read-modify-write，选 `-instcombine-max-iterations=1`，得 1.083x。
- **Source**：round 4 达到最佳 **2.026x**，之后几轮都没超过，round 5 退回 1.897x。LLM 改写了 A 的访问模式，合并了两次遍历。
- **问题**：轻微的不稳定，round 3 退化，说明 LLM 的改写质量有随机波动。

### bicg
- **Param**：识别 20 次 GVN miss + 12 次 LICM miss，选 `-slp-look-ahead-users-budget=8`，得 1.031x。
- **Source**：round 2 出现 precision_error（1/240 元素超标，rel=4.28e-4，FP 重排序导致），rounds 6-8 全部 compile_error（LLM 使用了无效的 pragma 语法如 `ivdep`）。最终 **3.713x**（round 5）。
- **问题**：后期 compile_error 连发，源于 LLM 在越来越激进的优化中引入了 GCC/Intel 专属 pragma，clang 不认识。

### cholesky
- **Param**：识别 SLP+LV 向量化失败 + 40 次 GVN miss，选 `-amdgpu-unroll-threshold-if=150`，仅 1.012x。
- **Source**：**全部 8 轮失败**。round 1 甚至改变了输出数组结构（size mismatch: 7260→239），rounds 2-6、8 均为 518/7260 元素出错（abs=1e-2），round 7 通过但慢了 3.3%。
- **问题**：Cholesky 分解有严格的 loop-carried in-place 依赖（`A[i][k]` 在同一 pass 内被后续步骤使用）。LLM 每次都尝试并行化内层 reduction，破坏了分解的语义。这类算法 LLM 存在根本性盲区。

### correlation
- **Param**：识别列优先访问导致 bandwidth 瓶颈，选 `-licm-max-num-uses-traversed=64`，仅 1.001x（几乎无效）。
- **Source**：round 1 即达 15.262x，round 2 进一步到 16.393x，round 7 达最终最佳 **16.454x**。rounds 5-6 大幅退化（3.1~3.3x）说明 LLM 偶尔走了错误方向但能恢复。LLM 将列主序的 mean/stddev/corr 计算全部改为行主序 + restrict 指针，使编译器充分向量化。
- **问题**：部分轮退化明显，说明 LLM 的迭代并不总是单调改进的。

### covariance
- **Param**：识别列主序 data 矩阵访问 + 向量化失败，选 `-pragma-vectorize-scev-check-threshold=64 -slp-look-ahead-users-budget=8`，得 1.091x。
- **Source**：**7/8 轮失败**（精度错误），仅 round 2 通过但速度 0.999x 无提升。失败模式固定：12/6400 元素，abs=1e-2，rel=1.58e-4（刚好超过 epsilon=1e-4）。LLM 将循环从 `i→j→k` 重排为 `k→i→j` 引发 FP 加法顺序变化。
- **问题**：误差极小（刚好越界），若 epsilon 放宽至 1e-3 即可通过。Source 优化实际上是合理的（手动验证 SMALL_DATASET 下误差为 0），但严格的精度门槛拒绝了所有尝试。

### deriche
- **Param**：识别垂直扫描列访问 stride-H cache miss，选 `-gvn-max-num-deps=1000`，得 1.122x。
- **Source**：round 2 达最佳 **2.221x**，之后 rounds 3-8 均未超过，最终 log 在 round 8 被截断（未记录结果）。LLM 改写了列方向递归为行缓存友好版本。
- **问题**：改进在 round 2 已饱和，后续轮反复尝试但无更好方案。

### doitgen
- **Param**：识别 C4 非单位 stride + 44 次 LICM miss，选 `-licm-max-num-uses-traversed=16 -amdgpu-unroll-threshold-if=600`，得 1.047x。
- **Source**：round 1 即达 **8.110x**（最终最佳），round 3 出现 all-zeros bug（输出全为 0，逻辑错误），round 7 compile_error，其余轮 7.0~7.7x。LLM 将内层 `C4[s][q]` 的转置访问改为行优先，大幅降低 cache miss。
- **问题**：round 3 all-zeros 是严重的逻辑错误（LLM 可能引入了零初始化覆盖了计算结果），但幸运地被正确性检查捕获。

### durbin
- **Param**：识别 FP 重排序阻止 reduction 向量化，选 `-loop-distribute-scev-check-threshold=64`，得 1.100x。
- **Source**：round 3 达最佳 **3.894x**（0.83ms），之后几轮均退化。LLM 对 Durbin 递归算法重新组织了 sum 的累加方式，允许编译器向量化。
- **问题**：round 3 之后无法再改进，且反复退化，说明 Durbin 的递归结构限制了进一步优化空间。

### fdtd-2d
- **Param**：**param 是本 benchmark 最强贡献**，识别三个分离的 2D grid sweep 导致 bandwidth 瓶颈，选 `-amdgpu-unroll-threshold-if=150 -loop-load-elimination-scev-check-threshold=3`，得 **1.393x**。
- **Source**：round 1 compile_error，round 2 退化（0.735x），rounds 3-7 缓慢爬升，round 7 达 **1.850x**。LLM 多次尝试 loop fusion 合并三个 sweep，但 fdtd 的依赖关系使 fusion 受限。
- **问题**：LLM 理解了 bandwidth 瓶颈但 source 改进有限，param 的贡献反而更大。

### floyd-warshall
- **Param**：识别 row-k 被重复流式读取，选 `-slp-threshold=0 -loop-distribute-scev-check-threshold=32`，得 1.094x。
- **Source**：rounds 2、4、5 均比 baseline 慢（0.769x~0.985x），最终最佳仅 **1.182x**（round 1）。LLM 尝试改变 k-i-j 循环顺序或缓存 `path[k]` 行，但 Floyd-Warshall 的三重嵌套全对最短路径本质上难以 cache-优化。
- **问题**：多次退化说明 LLM 对这类"天然难优化"算法效果不稳定，改写不一定有益。

### gemm
- **Param**：识别 B 矩阵被反复从 DRAM 读取（无 cache tiling），选 `-gvn-hoist-max-bbs=20`，仅 1.031x；但 retune 阶段找到 `-amdgpu-unroll-threshold-if=300 -unroll-threshold=600` 组合达 1.487x（retune 在 source 的基础上应用）。
- **Source**：round 1 达 1.352x（最终最佳），之后 7 轮全部没有超越。LLM 识别了需要 cache tiling，但实现的 tiling 方案未能超过 round 1。
- **问题**：gemm 是最经典的矩阵乘法，LLM 没有生成真正高性能的 blocked GEMM，说明 LLM 对这类深度优化场景能力有限。retune 的 param 反而比 source 提升更大。

### gemver
- **Param**：识别 x2 更新的列主序 stride-N 访问，选 `-slp-threshold=-1`，得 1.021x。
- **Source**：steady improvement：1→2→3 轮逐步到 **3.499x**，round 4 大幅退化（1.425x），之后也未能超越 round 3。LLM 将 `A[j][i]` 的转置访问改为行缓存友好形式。
- **问题**：round 4 退化说明 LLM 在某一轮做出了错误的优化方向选择。

### gesummv
- **Param**：识别 36 次 GVN miss + bandwidth 瓶颈，选 `-gvn-hoist-max-bbs=10 --slp-threshold=-1`，得 1.055x。
- **Source**：仅 round 1 通过（**1.686x**），rounds 2-8 全部同一 precision_error（5/90 元素，rel=1.64e-4，FP 重排序），7 轮相同错误说明 LLM 无法绕过此问题。
- **问题**：与 covariance 类似——FP 加法重排序引起极小误差，但严格 epsilon 拒绝所有后续尝试。LLM 每轮都重复同样的向量化策略，没有意识到问题所在。

### gramschmidt
- **Param**：识别 SLP 被 cost threshold 阻断，选 `-slp-threshold=-2`，仅 1.022x。
- **Source**：rounds 1-2 快速攀升（13.4x → 16.2x），round 3 出现严重 precision_error（abs=31，应是 Gram-Schmidt 正交化的数值不稳定），rounds 4-5 继续改进到 **16.727x**（round 5，最终最佳），rounds 6-7 再次触发相同的严重错误，round 8 回落。
- **问题**：Gram-Schmidt 对数值精度敏感，LLM 某些优化（如改变归一化顺序）会导致正交基方向错误，表现为巨大 abs_diff（~31）。但多数轮次的改写是正确的，最终取得全场最高 speedup。

### heat-3d
- **Param**：**param 是主要贡献**，识别 3D stencil 的大 stride 邻居访问，选 `-loop-load-elimination-scev-check-threshold=4`，得 **1.281x**。
- **Source**：8 轮中 rounds 3-4 严重退化（0.45x~0.57x），round 6 勉强 1.023x（最佳 source），整体表现很差。LLM 尝试了 loop tiling 但 heat-3d 的内存访问模式太规则，改写反而引入了 overhead。
- **问题**：3D stencil 是 bandwidth-bound kernel，source 改写能做的有限，param 调整反而更有效。退化最大达 0.45x，LLM 的激进 tiling 尝试完全失败。

### jacobi-1d
- **Param**：识别两个全数组 sweep（A→B, B→A）bandwidth 瓶颈，选 `-slp-threshold=-2 -instcombine-max-iterations=5`，得 1.071x。
- **Source**：rounds 1-6 全部慢于 baseline（最差 0.348x），仅 round 7 勉强 1.040x。Jacobi-1d 是最简单的 stencil，几乎没有优化空间，LLM 的改写反而引入 overhead。
- **问题**：kernel 太小（baseline 仅 0.47ms），任何额外的 function call overhead 或循环结构变化都会带来相对大的损失。

### jacobi-2d
- **Param**：识别 SLP miss + 大量 GVN miss，选 `-slp-look-ahead-users-budget=4`，得 1.114x。
- **Source**：round 2 勉强 1.002x，其余 7 轮全部比 baseline 慢（最差 0.562x）。LLM 反复尝试 tiling/向量化，但 Jacobi-2d 的双 buffer 交替 stencil 结构使得改写难以提升。
- **问题**：与 jacobi-1d 类似，编译器在 -O3 下已做了很好的向量化，source 改写空间极小，LLM 的尝试多数有害。

### ludcmp
- **Param**：识别前向/后向代入循环未向量化（FP 评估顺序约束），选 `-pragma-vectorize-scev-check-threshold=128 -licm-max-num-uses-traversed=64`，得 1.126x。
- **Source**：round 1 达 4.373x，round 2 小幅提升到 4.424x（最终最佳前），rounds 3-8 未能超越，但均在 4.2~4.4x 之间稳定。最终（retune 后）**4.525x**。LLM 识别了三角矩阵的列访问问题，改写为行优先并手动展开。
- **问题**：收敛较快，round 2 之后无明显进展，LLM 在这类 solver 上改进空间受限。

### lu
- **Param**：识别 `A[k][j]` 列主序访问阻止向量化，选 `-amdgpu-unroll-threshold-if=150 -instcombine-max-iterations=6`，得 **1.151x**。
- **Source**：**全部 8 轮 precision_error**（1300/14400 元素，abs=1e-2），模式完全一致。LLM 对 LU 分解的 in-place 依赖（`A[i][j] -= A[i][k]*A[k][j]`，k 循环内 `A[i][k]` 已被更新）视而不见，每次都尝试向量化 k-loop，导致依赖被破坏。round 2 出现 `#pragma clang loop ivdep` 语法（告诉编译器忽略依赖），直接暴露了 LLM 的意图。
- **问题**：与 cholesky 同类，LLM 不理解 in-place 三角分解的语义约束，8 轮 24 次尝试全部语义错误。

### mvt
- **Param**：未在 log 最终 summary 中显示 param 值，但 retune 找到 `-slp-look-ahead-users-budget=16 -amdgpu-unroll-threshold-if=600`。
- **Source**：逐轮改进，从 round 1 的 2.637x 到 round 6 的 **4.143x**（最终最佳），round 7 出现 precision_error（4/242 元素，FP 重排序），round 8 回落到 4.011x。LLM 识别了 x2 更新的 `A[j][i]` 转置访问，改为转置循环。
- **问题**：round 7 的 precision_error 打断了迭代链，说明 LLM 在某个方向走过头引入了 FP 敏感操作。

### nussinov
- **Param**：识别 `table[k+1][j]` 列主序 stride 访问，选 `-instcombine-max-iterations=3`，仅 1.028x。
- **Source**：轮次稳定，最终 round 8 达 **12.533x**。LLM 改写了 DP 内层 k-loop 的访问顺序（转置访问改为行缓存友好），这是典型的 DP cache 优化。
- **问题**：无明显问题，是本次实验中最稳定的高倍率 benchmark 之一。

### seidel-2d
- **Param**：识别 bandwidth 瓶颈（每 time step 全数组流式读写），选 `-slp-look-ahead-users-budget=8`，仅 1.027x（retune 实际测出 1.127x 但最终 summary 记录为 1.027x）。
- **Source**：**6/8 轮 precision_error**（337/14400 元素，abs=1e-2），round 2 compile_error（LLM 直接把解释文字写进 C 文件），round 7 勉强 1.017x（最终记录的 source best）。
- **问题**：Gauss-Seidel 的语义就是"原地更新，后面的元素用已更新的前面元素"。LLM 反复将其改为 Jacobi 风格（snapshot 整个数组再更新），这是不同的算法，结果不同。16.5 小时中大部分时间耗在这个 kernel 上，是本次实验最大的失败案例。

### symm
- **Param**：识别 `C[k][j]`、`B[k][j]` 的 stride-N 访问，选 `-licm-max-num-uses-traversed=128`，得 1.116x。
- **Source**：round 1 达 4.587x，之后 rounds 2-5 均退化（2.9~3.4x），round 6 跳升到 5.260x，round 7 回落。最终 retune 后 **6.093x**。LLM 将内层 k-loop 的转置访问改为行主序并引入临时变量缓存。
- **问题**：非单调改进，round 6 的大幅提升是 LLM 在前几轮失败后找到的新策略，说明多轮迭代的重要性。

### syr2k
- **Param**：识别 `A[j][k]`、`B[j][k]` stride-M 访问，选 `-gvn-max-num-deps=200 -pragma-vectorize-scev-check-threshold=64`，得 1.063x。
- **Source**：**rounds 1-6 全部 precision_error**（FP 重排序，19~145 元素超标），rounds 7-8 才通过并达 **5.537x**。LLM 前 6 轮尝试了某种加速方案但引入了 FP 重排；rounds 7-8 换了保守写法，避免了精度问题。
- **问题**：前 6 轮的精度错误说明 LLM 存在一个持续的错误倾向（可能是引入了 SIMD 对称矩阵填充），直到 round 7 才被纠正。

### syrk
- **Param**：识别 `A[j][k]` stride-M cache miss，选 `-slp-threshold=-2`，得 1.035x。
- **Source**：rounds 1-3 仅 2x 左右，round 4 突然跳升至 **7.853x**（最终最佳），rounds 5-8 回落至 6.9~7.5x。LLM 在 round 4 找到了将转置访问改为行主序 + 向量化友好排列的方案。
- **问题**：round 4 的突破说明迭代探索的价值，但也说明最优解的出现有一定随机性。

### trisolv
- **Param**：识别外层循环递推依赖（`x[i]` 依赖所有前序 `x[j]`）阻止向量化，选 `-licm-max-num-uses-traversed=128`，得 1.202x。
- **Source**：round 1 退化（0.737x），round 2 微小改进，round 3 达 **1.818x**（最终最佳），之后 5 轮均未超越。LLM 识别了 O(N²) 内存流量瓶颈，在保持依赖的前提下优化了 L 矩阵的访问模式。
- **问题**：外层递推依赖是根本限制，LLM 找到了有限的优化空间后便饱和。

### trmm
- **Param**：识别内层 k-loop strided access，选 `-loop-load-elimination-scev-check-threshold=2`，仅 1.014x。
- **Source**：rounds 1-2 就达 11x+，round 5 达 **13.032x**（最终最佳），rounds 6-8 全部 compile_error（LLM 在后期引入了无效 pragma 或语法错误）。
- **问题**：后三轮 compile_error 连发，与 bicg 类似，是 LLM 在找不到新突破时引入了错误语法。但 round 5 已经找到了最优解，后续失败不影响结果。

---

## 问题模式总结

| 问题类型 | 涉及 benchmark | 根因 |
|---------|--------------|------|
| **Loop-carried dependency** | lu, cholesky, seidel-2d | LLM 对 in-place 算法的依赖关系不敏感，将顺序算法错误并行化 |
| **FP 重排序 precision_error** | covariance, gesummv, syr2k（前6轮）, mvt round7 | 向量化/循环重排改变加法顺序，误差极小但超过 epsilon=1e-4 |
| **Compile error（无效 pragma）** | bicg, trmm, doitgen, 3mm | LLM 使用 GCC/Intel 专属语法（`ivdep`）或直接输出解释文字 |
| **性能退化** | adi, heat-3d, jacobi-1d/2d, floyd-warshall | Stencil/bandwidth-bound kernel，LLM 的 tiling 尝试引入 overhead |
| **突破性改进** | correlation, gramschmidt, trmm, nussinov | 访问模式从列主序→行主序，消除了主要 cache miss 瓶颈 |

## Param 调优规律

- **高效 flag**：`-instcombine-max-iterations`、`-slp-threshold`、`-licm-max-num-uses-traversed`、`-loop-load-elimination-scev-check-threshold` 是最常见的有效 flag。
- **Param 独赢场景**：fdtd-2d（1.393x）、heat-3d（1.281x）、trisolv（1.202x）——均为 source 优化受限的 stencil/solver kernel。
- **Param 几乎无效**：correlation（1.001x）、adi（1.016x）——LLM 选的 flag 对这些 kernel 没有实质影响。
- **Retune 价值**：gemm 的 retune 在 source 改写基础上找到了比 initial param search 更好的 flag 组合（1.031x → 1.487x），说明 source + retune 的复合收益不可忽视。
