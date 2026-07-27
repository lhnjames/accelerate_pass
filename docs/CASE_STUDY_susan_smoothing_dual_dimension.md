# Case Study：automotive_susan_smoothing —— 代码重写和编译flag两个维度都独立、稳定生效

日期：2026-07-27。目标：找一个"改代码有真实收益、调编译flag也有明显且稳定收益、两者组合也work"的case，
且必须在n=9严格交替测量下都站得住（而不是n=3小样本下看似显著、实际是噪声）。

这是经过对30个PolyBench kernel + cBench候选、6种flag策略系统搜索、9次尝试后**第一个同时满足全部条件的candidate**。
过程中另外发现了两个重要的方法论教训（详见第5节），特此记录。

## 1. 背景：这是真实的生产级视觉算法代码，不是toy kernel

`automotive_susan_smoothing`来自SUSAN（Smallest Univalue Segment Assimilating Nucleus）图像处理算法
（Stephen Smith, Oxford, 1995），是cBench真实收录的产品级C代码。本次测试跑的是它的"大高斯掩码"分支
（`three_by_three=0`，默认路径，dt=4.0 → mask_size=7，即每个像素要处理一个15×15=225元素的邻域）：

```c
for (i=mask_size; i<y_size-mask_size; i++)
  for (j=mask_size; j<x_size-mask_size; j++)
  {
    area = 0; total = 0; dpt = dp;
    ip = in + ((i-mask_size)*x_size) + j - mask_size;
    centre = in[i*x_size+j];
    cp = bp + centre;
    for (y=-mask_size; y<=mask_size; y++)
    {
      for (x=-mask_size; x<=mask_size; x++)
      {
        brightness = *ip++;
        tmp = *dpt++ * *(cp-brightness);   /* 数据依赖的LUT查表：双边滤波的非线性核心 */
        area += tmp;
        total += tmp * brightness;
      }
      ip += increment;
    }
    tmp = area-10000;
    *out++ = (tmp==0) ? median(in,i,j,x_size) : ((total-(centre*10000))/tmp);
  }
```

**关键结构性事实**：`area`/`total`/`tmp`/`brightness`全部是**整型**（`TOTAL_TYPE`在非PPC构建下就是`int`，
源码第293行`typedef int TOTAL_TYPE;`），不是浮点。`*(cp-brightness)`是一次**数据依赖的查表**（bilateral滤波
的非线性核心：空间距离权重`dpt`乘以一个由亮度差决定的查表值），这种gather式访存本质上不适合SIMD向量化，
不管怎么重排代码、加不加`restrict`都改变不了这一点——真正能撬动性能的不是"消除别名让向量化器工作"，
而是别的方向。

## 2. 两个独立维度分别怎么改

### 2.1 代码重写：整数累加器拆分（打破ILP瓶颈，不是重排浮点）

原代码的`area +=`/`total +=`是横跨225次迭代的**单一串行累加链**——下一次加法必须等上一次算完，
这是经典的指令级并行(ILP)受限模式。由于三个累加变量都是**整数**，拆成两条独立累加链、最后再合并，
在数学上是**严格精确的重结合**（整数加法满足结合律，不像浮点重排那样有精度风险），可以放心做：

```c
int area0, area1, b0, b1, t0, t1, xp;
TOTAL_TYPE total0, total1;
int xcount = mask_size*2 + 1, xpairs = xcount/2, xrem = xcount%2;
...
for (y=-mask_size; y<=mask_size; y++)
{
  area0 = 0; area1 = 0; total0 = 0; total1 = 0;
  for (xp = 0; xp < xpairs; xp++)
  {
    b0 = *ip++; t0 = *dpt++ * *(cp-b0); area0 += t0; total0 += t0*b0;
    b1 = *ip++; t1 = *dpt++ * *(cp-b1); area1 += t1; total1 += t1*b1;
  }
  if (xrem) { b0 = *ip++; t0 = *dpt++ * *(cp-b0); area0 += t0; total0 += t0*b0; }
  area += area0 + area1;
  total += total0 + total1;
  ip += increment;
}
```

### 2.2 编译flag：`-flto`（链接时优化）

`-ffast-math`对这个kernel完全无效（0.999x）——因为核心计算是整数运算，没有浮点重排空间可利用，这也印证了
"选对flag要匹配kernel的真实瓶颈类型"。`-march=native`、`-funroll-loops`等也都在噪声水平（0.97x~1.03x）。
唯一有实质效果的是`-flto`——SUSAN源文件有上千行、几十个函数（`median`、`enlarge`等辅助函数与热循环分散
在同一文件但跨越较远的代码距离），LTO带来的跨函数边界优化（更激进的内联/别名分析）是这里真正的收益来源，
而不是任何向量化相关的改动。

## 3. 严格验证方法与结果（全部在同一台机器dss-dgx-a、同一base binary、同一空闲核心上完成）

**注**：本节数据是最终版——第一版曾把flags-only的测量错误地放在了dgx-b上（其余三个在dgx-a），
已按要求全部重新在dgx-a、同一个baseline二进制、同一个pin_cpu=8上重新构建、重新测量。

### 3.1 编译命令（四个二进制，源目录/utils目录相同，仅source文件和extra_flags不同）

```
utils = CBench_shim_root/cbench-automotive-susan_smoothing/CBench_shim/utilities
src_dir = CBench_shim_root/cbench-automotive-susan_smoothing/CBench_shim/kernels/automotive_susan_smoothing

baseline:      clang -O3 -std=gnu99 -I$utils -I$src_dir -DLARGE_DATASET -DPOLYBENCH_TIME \
                 automotive_susan_smoothing.c $utils/polybench.c -o ss2_base -lm
flags-only:    同上 + -flto                                    -o ss3_flags
rewrite-only:  同上，但源文件替换为累加器拆分版（见3.4 diff）      -o ss2_rewrite
rewrite+flags: 累加器拆分版 + -flto                              -o ss2_both
```
（实际由`tune_param.compile_binary()`统一生成，上面是其内部拼出的等价命令。）

### 3.2 正确性：hash模式，字节级精确匹配（同机、同一baseline二进制）

cBench的这套shim harness用`hash`模式做正确性验证（对输出做精确哈希比较，不是数值容差比较）——
比PolyBench的numeric+epsilon容差验证更严格：只要有一个字节不同就会被拒绝。

```python
from src.correctness import detect_correctness_mode, check_correctness
mode = detect_correctness_mode('/tmp/ss2_base')            # -> 'hash'
check_correctness('/tmp/ss2_base', '/tmp/ss3_flags',   mode)  # (True, '')
check_correctness('/tmp/ss2_base', '/tmp/ss2_rewrite', mode)  # (True, '')
check_correctness('/tmp/ss2_base', '/tmp/ss2_both',    mode)  # (True, '')
```

三个候选全部通过。（正确性验证之所以能做到字节级精确，正是因为2.1节提到的"整数运算+整数重结合精确
等价"这个结构性事实——如果是浮点kernel，字节级hash比较通常做不到，需要退化到数值容差比较。）

### 3.3 计时：n=9交替测量，同一台机器(dss-dgx-a)、同一baseline二进制、同一pin_cpu=8

测量方法：`optimize.py`的`confirm_result_external(ref_bin, opt_bin, runs=9, pin_cpu=8)`——baseline
与候选交替测量9轮，取配对比值的中位数（避免探索期"最好单次"的选择性偏差）。

| 候选 | baseline中位数 | 候选中位数 | 确认加速比 | n_positive | IQR | best_cv |
|---|---|---|---|---|---|---|
| flags-only (`-flto`) | 28.699ms | 22.361ms | **1.2820x** | 9/9 | [1.2785, 1.2900] | 0.44% |
| rewrite-only | 28.682ms | 27.945ms | **1.0275x** | 9/9 | [1.0214, 1.0297] | 0.34% |
| rewrite+flags(both) | 28.721ms | 22.970ms | **1.2559x** | 9/9 | [1.2371, 1.2657] | 0.79% |

### 3.4 rewrite的完整diff（真实代码，未删减）

```diff
@@ -731,6 +731,22 @@
 /* }}} */
     /* {{{ main section */
 
+  {
+    /* area/total are plain int accumulators (see TOTAL_TYPE probe above),
+       so splitting the inner 2*mask_size+1-wide row-sum into two
+       independent accumulator streams is an EXACT reassociation (integer
+       addition is strictly associative/commutative, unlike the float
+       cases elsewhere in this project) -- not an approximation. This
+       just breaks the single serial add-chain per row into two
+       independent chains to expose more ILP; the two streams are merged
+       back into area/total once per row, same as the original merged
+       once per y-iteration. */
+    int xcount = mask_size*2 + 1;
+    int xpairs = xcount / 2;
+    int xrem   = xcount % 2;
+    int area0, area1, b0, b1, t0, t1, xp;
+    TOTAL_TYPE total0, total1;
+
   for (i=mask_size;i<y_size-mask_size;i++)
   {
     for (j=mask_size;j<x_size-mask_size;j++)
@@ -743,13 +759,27 @@
       cp = bp + centre;
       for(y=-mask_size; y<=mask_size; y++)
       {
-        for(x=-mask_size; x<=mask_size; x++)
-	{
-          brightness = *ip++;
-          tmp = *dpt++ * *(cp-brightness);
-          area += tmp;
-          total += tmp * brightness;
+        area0 = 0; area1 = 0; total0 = 0; total1 = 0;
+        for (xp = 0; xp < xpairs; xp++)
+        {
+          b0 = *ip++;
+          t0 = *dpt++ * *(cp-b0);
+          area0 += t0;
+          total0 += t0 * b0;
+
+          b1 = *ip++;
+          t1 = *dpt++ * *(cp-b1);
+          area1 += t1;
+          total1 += t1 * b1;
         }
+        if (xrem) {
+          b0 = *ip++;
+          t0 = *dpt++ * *(cp-b0);
+          area0 += t0;
+          total0 += t0 * b0;
+        }
+        area += area0 + area1;
+        total += total0 + total1;
         ip += increment;
       }
       tmp = area-10000;
@@ -759,6 +789,7 @@
         *out++=((total-(centre*10000))/tmp);
     }
   }
+  }
 
   free(dp);
```

### 3.5 三条腿是否都真实、稳定

- **rewrite-only**：2.75%虽然量级不大，但9/9次测量全部为正、IQR跨度很窄（cv 0.34%），是一个真实、
  可复现的效果，不是噪声。
- **flags-only**：28.2%，9/9次全部为正，IQR跨度窄（cv 0.44%），同样稳定可信。
- **both**：25.6%，同样9/9次全部为正、IQR较窄（cv 0.79%）——**比flags-only单独还低一点**（1.2559x vs
  1.2820x），说明这两个优化不是简单乘法叠加（理论上独立叠加应该接近1.0275×1.2820≈1.317x），累加器拆分
  改变了LTO内联/调度决策的具体形式，产生了轻微的负向干扰。但"both"仍然是一个真实、显著优于baseline的
  结果，只是没有做到超加性(superadditive)——这是一个诚实的、值得在论文里如实说明的细节，不影响
  "两个维度各自独立有效"这个核心论点。

## 4. 为什么这个case比之前失败的候选更有说服力

这是唯一一个满足"rewrite-only在n=9下9/9次为正"的候选——2mm、durbin(两版)、syrk都在这一步失败
（rewrite或flags某一条腿在n=9下n_positive<9/9，IQR跨过1.0）。关键区别：

1. **代码重写针对的是真实、可清楚定位的瓶颈**（串行累加链限制ILP），而不是泛泛的"改善缓存/别名"——
   前几次失败的durbin/2mm重写要么完全没找到真实瓶颈方向，要么改动本身干扰了编译器已有的优化路径。
2. **整数运算避免了浮点重排的精度风险**，使得这个改动可以做到字节级精确正确，不需要在"改动幅度"和
   "正确性风险"之间做权衡。
3. **flag（`-flto`）针对的瓶颈跟代码重写针对的瓶颈是两个完全不同的维度**（前者是ILP/累加链，后者是
   跨函数边界的内联/别名分析），两者天然不容易互相踩踏（虽然"both"确实有一点点负向干扰，但没有像
   durbin那样出现"组合比单独flags还明显更差"的情况）。

## 5. 过程中发现的两个方法论教训（已撤回的错误结论）

### 5.1 "2mm重写在LARGE规模下算错83%" —— 已撤回，是我自己的方法论bug
最初怀疑2mm的restrict+分块重写在`-DPOLYBENCH_TIME`构建、LARGE_DATASET下算错了83%。根因排查后发现：
`-DPOLYBENCH_TIME`模式下`polybench_print_instruments`只打印墙钟耗时（`polybench.h`：
`#define polybench_print_instruments polybench_timer_print();`），**不打印数组内容**。用
`-DPOLYBENCH_DUMP_ARRAYS`重新验证后，2mm的重写完全正确。

### 5.2 "durbin在LARGE规模下数值病态" —— 已撤回，是同一个bug的另一次误判
最初用同一个`-DPOLYBENCH_TIME`二进制自己跟自己比较，发现"结果不一致"，误判为Levinson-Durbin递归
本身数值不稳定。同5.1根因：比较的是两次运行的计时噪声，不是真实计算结果。用`-DPOLYBENCH_DUMP_ARRAYS`
重新验证后，durbin自己跟自己完全一致。

**教训**：任何"验证正确性"的工作，必须确认比较的是真实数组/文件输出（`-DPOLYBENCH_DUMP_ARRAYS`或
cBench的hash模式），而不是`-DPOLYBENCH_TIME`构建的标准输出——后者只有一个计时数字，拿它做数值比较
在方法论上从一开始就是错的。

## 6. 附：搜索过程中失败的候选（供对照）

| kernel | 稳定的维度 | 崩溃/无效的维度 |
|---|---|---|
| 2mm | rewrite-only 8.883x (n_positive=9/9) | flags-only（官方选择）：n=3显著1.2449x → n=9变噪声1.007x(5/9) |
| durbin | flags-only(`-ffast-math`) 1.626x-1.392x (n_positive=9/9) | rewrite-only（官方n=3显著1.8647x）→n=9变噪声1.007x(5/9)；后续两版手写rewrite（分支版/无分支版）均无独立效果(confirmed 0.95x~0.99x)，且与flags组合后比单独flags更差 |
| syrk | rewrite-only 3.054x (n_positive=9/9) | flags-only：我自选`-funroll-loops -mprefer-vector-width=512`筛选1.217x→n=9噪声0.929x(4/9)；官方选择`-licm-max-num-uses-traversed=32`n=3显著1.0812x→n=9噪声0.945x(3/9) |
| seidel-2d | flags-only(`-ffast-math`) 2.38x（仅n=2筛选，未做n=9严格复测） | rewrite-only官方n=3已标记不显著(1.003x)；我设计的"滑动列和"rewrite在1e-4严格阈值下未通过正确性（MEDIUM/LARGE均约1%相对误差），已按标准淘汰，未继续放宽阈值 |

这份附表本身也是一个有价值的发现：**在这批benchmark的规模和结构下，大多数kernel似乎只有一个真正的瓶颈
维度**，susan_smoothing能同时兼顾两个维度、且都能做到字节级精确验证，是9次尝试里唯一的例外。
