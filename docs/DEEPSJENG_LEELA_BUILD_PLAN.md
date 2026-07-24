# 531.deepsjeng_r / 541.leela_r 构建调研——multi-TU manifest 素材

**日期**: 2026-07-21
**性质**: 只读调研文档。没有修改任何代码（`scripts/gen_spec_multitu.py`、`src/build_manifest.py` 等一律未动）、没有复制/同步任何真实源码到本仓库、没有编译、没有计时、没有改远程任何文件、没有提交 git。
**调研范围**: SSH 到 `ubuntu@132.145.22.86`，只读读取 `/home/hanning/spec2017/benchspec/CPU/{531.deepsjeng_r,541.leela_r}/` 下的 `Spec/object.pm`、`src/`、`data/`，以及 `docs/NAMD_CXX_BUILD_PLAN.md` 里已经读过的 `/home/hanning/spec2017/config/comet-clang.cfg.2026-07-08T192459`（全局 `CXX`/`EXTRA_PORTABILITY` 取值来源，这两个 benchmark 都不在该 cfg 的 `__HASH__` 历史记录里，说明它们之前从未被这套配置真正编译过，下面的取值只来自全局默认段 + 各自 `object.pm`，还没有被"真实编译过一次"这件事验证过，比 namd_r 的证据链弱一档）。
**格式承接**：沿用 `docs/NAMD_CXX_BUILD_PLAN.md` 已经建立的证据组织方式；本文档产出的 metadata 字段名直接对应 `src/build_manifest.py` 的 schema（`sources`/`include_dirs`/`defines`/`compile_flags`/`link_flags`/`cxx_standard`/`runtime`），供后续（不在本次任务范围内的）生成器改造直接取用。

---

## 1. 531.deepsjeng_r（国际象棋引擎，Deep Sjeng）

### 1.1 语言与源文件

`object.pm: $benchlang = 'CXX'`，纯 C++。`@sources` 共 **20 个 `.cpp` 文件**，全部在 `src/` 下，**无子目录**：

```
attacks.cpp  bitboard.cpp  bits.cpp  board.cpp  draw.cpp  endgame.cpp
epd.cpp  generate.cpp  initp.cpp  make.cpp  moves.cpp  neval.cpp
pawn.cpp  preproc.cpp  search.cpp  see.cpp  sjeng.cpp  state.cpp
ttable.cpp  utils.cpp
```

`src/` 下另有 24 个 `.h` 头文件（同样全部平铺，无子目录）+ 1 个 `Makefile`（标准 SPEC 模板 stub，无实际信息）。

### 1.2 入口

`sjeng.cpp` 定义 `int main(int argc, char *argv[])`。逻辑：`start_up()` / `init_parameters()` / 一系列初始化后，**要求 `argc == 2`**（否则打印 "Please specify the workfile." 并返回 `EXIT_FAILURE`），随后调用 `run_epd_testsuite(&gamestate, &state, argv[1])`（定义于 `epd.cpp`）。`run_epd_testsuite` 内部对 `argv[1]` 做的是**普通 `fopen(testname, "r")`**——传绝对路径可以直接工作，跟 nab_r 不同，**不需要 rundir/chdir**。

### 1.3 编译参数

| 来源 | 取值 |
|---|---|
| `object.pm: $bench_flags` | `-DSMALL_MEMORY -DSPEC_AUTO_SUPPRESS_OPENMP` |
| `initp.cpp` 里的硬约束 | `#ifdef SMALL_MEMORY ... #else ... #error Need to specify SMALL_MEMORY or BIG_MEMORY.` —— **`SMALL_MEMORY` 是必需项，不定义就直接编译报错**，不是可选优化开关。 |
| `Makefile.defaults: CPUFLAGS`（全 benchmark 通用）| `-DSPEC -DNDEBUG` |
| 全局 cfg: `EXTRA_PORTABILITY`（64-bit 分支，deepsjeng_r 没有专属 `PORTABILITY` 覆盖）| `-DSPEC_LP64` |
| 全局 cfg: `CXX` | `clang++-N -std=gnu++03`（历史 cfg 原文是错误的 `clang-11++` 可执行文件名，语义上取其 `-std=gnu++03` 部分；**deepsjeng_r 本身不在该 cfg 曾经真实编译过的名单里，`gnu++03` 对它是否真的必要/够用没有被验证过**，只是继承自"这个 cfg 对所有 C++ benchmark 都统一套用同一个 CXX 标准版本"这个全局约定，建议实际生成/编译时用 `gnu++03` 起步，如遇标准相关编译错误再按报错逐步放宽到 `gnu++11`/`gnu++14`）。 |
| 链接 | 源码里没有 `pthread`/`#pragma omp`/`omp.h` 引用，`-lm` 应该够用，跟现有约定一致。 |

### 1.4 运行方式（workload / argv / 数据依赖）

`object.pm::invoke()`：对每个匹配 `(.*)\.txt$` 的输入文件，构造 `{command: exe, args: ["$name.txt"], output: "$name.out", error: "$name.err"}`。跟 namd_r 不同的是——**这里没有"参数字符串文件 + 共享数据文件"的两层结构**，每个 workload 尺寸自己就是一个独立、自包含的 EPD 文本文件，不依赖 `data/all/` 下的共享文件：

| workload | 输入文件 | 行数 |
|---|---|---|
| test | `data/test/input/test.txt` | 2 行 |
| train | `data/train/input/train.txt` | （未详查，规模介于 test/ref 之间）|
| refrate | `data/refrate/input/ref.txt` | 24 行 |

argv 本身极简：`["kernel_deepsjeng_r", "<绝对路径>/test.txt"]`（或 `ref.txt`）——生成时只需要把 `object.pm` 给的相对文件名换成绝对路径，同样不需要 rundir。

程序 stdout/stderr 具体格式未在本次调研中逐行核实（`myprintf` 输出，附带 EPD 测试的通过/失败统计），但 SPEC 自带的 `data/{test,refrate}/output/*.out` 参考输出文件可以直接作为数值/哈希正确性比对的基准。

### 1.5 可直接转 build_manifest 的字段草稿

```jsonc
{
  "version": 1,
  "name": "deepsjeng_r",
  "sources": [ /* sources/sjeng.cpp（entry，main 重命名+wrapper）+ 19 个其余 .cpp，逐字节复制，不拼接 */ ],
  "include_dirs": ["sources"],
  "defines": ["SPEC", "NDEBUG", "SPEC_LP64", "SMALL_MEMORY", "SPEC_AUTO_SUPPRESS_OPENMP"],
  "link_flags": ["-lm"],
  "cxx_standard": "gnu++03",          // 继承自全局 cfg 约定，未针对本 benchmark 单独验证过
  "workloads": {
    "test":    { "argv": ["kernel_deepsjeng_r", "{abs}/test.txt"] },
    "refrate": { "argv": ["kernel_deepsjeng_r", "{abs}/ref.txt"] }
  },
  "no_rundir_needed": true             // argv[1] 直接 fopen，跟 namd_r 一样简单
}
```

---

## 2. 541.leela_r（围棋引擎，基于 Leela）

### 2.1 语言与源文件——**比 namd_r/deepsjeng_r 都复杂，含嵌套的 header-only Boost 子树**

`object.pm: $benchlang = 'CXX'`，纯 C++。`@sources` 共 **21 个 `.cpp` 文件**：

```
FullBoard.cpp  KoState.cpp  Playout.cpp  TimeControl.cpp  UCTSearch.cpp
GameState.cpp  Leela.cpp  SGFParser.cpp  Timing.cpp  Utils.cpp
FastBoard.cpp  Matcher.cpp  SGFTree.cpp  TTable.cpp  Zobrist.cpp
FastState.cpp  GTP.cpp  MCOTable.cpp  Random.cpp  SMP.cpp  UCTNode.cpp
```

`src/` 下还有 22 个 `.h`（跟 `.cpp` 同级平铺）——**外加一个 `src/boost/` 子目录，412 个文件，全部是头文件（`find ... -name '*.cpp' -o -name '*.cc'` 结果为 0），是一份 vendor 进来的 header-only Boost 子集**（`preprocessor`/`mpl`/`tuple`/`type_traits`/`exception`/`utility`/`config`/`tr1`/`compatibility` 等模块，用于模板元编程，没有需要单独编译链接的 Boost 库主体）。

**这是 leela_r 相对 namd_r/deepsjeng_r 独有的一个架构级差异**：`boost/` 内部用的是 `#include <boost/xxx/yyy.h>` 这种保留相对目录结构的写法（例如 `boost/preprocessor/detail/foo.h` 必须仍然位于 `<include_root>/boost/preprocessor/detail/foo.h`），**不能像现有 `gen_spec_kernels.py`/`gen_spec_multitu.py` 对头文件的"全部拉平到一个目录"那套做法去处理**——那样会把 `boost/preprocessor/detail/xxx.h` 和 `boost/mpl/aux_/xxx.h` 等目录结构抹平，只要有同名文件出现在不同子目录（412 个文件里出现同名的概率不低）就会互相覆盖，而且就算不重名，`#include <boost/xxx/yyy.h>` 这种带相对路径的写法在拉平之后也解析不到。**结论：leela_r 的头文件复制策略必须是"保留 `src/` 内部原有目录结构做一次整体子树复制"，而不是"收集所有 `.h` 文件拉到一个平的目录"，`include_dirs` 只需要指向复制后的 `src/` 根（即 `boost/` 的父目录），不需要单独再列一层 `.../boost`。**

### 2.2 入口

`Leela.cpp` 定义 `int main(int argc, char *argv[])`（调研过程中 `grep 'int main'` 曾对 `GTP.cpp`/`GameState.cpp`/`TimeControl.cpp` 里的 `int maintime`/`int maintime, ...` 产生过假阳性匹配，已核实排除——这三个文件里都不是函数入口，只是形参名恰好以 "int main" 开头）。

逻辑：`argc < 2` 时打印 "No file specifided" 并返回 `EXIT_FAILURE`；否则 `filename.assign(argv[1])`，随后在一个 `for(;;)` 循环里 `sgftree->load_from_file(filename, counter++)` 反复读同一个文件（棋谱重放，每次编号递增）。`load_from_file` 内部预期也是普通路径打开，未见 chdir 依赖，同样**不需要 rundir**。

**发现一个必须在实际编译前验证的强约束**：`Leela.cpp`/`GTP.cpp`/`GameState.cpp`/`SGFParser.cpp`/`SGFTree.cpp` 五个文件都用到了 **`std::auto_ptr`**——这是 C++98/03/11/14 里的构造，**在 C++17 标准下被彻底移除（不是 deprecated warning，是编译期硬错误）**。这意味着 leela_r 对 C++ 标准版本的要求比"随便选一个"严格得多：必须显式传一个 C++17 之前的 `-std=`（`gnu++03`/`gnu++11`/`gnu++14` 均应该能保留 `auto_ptr`），**绝对不能用 clang++-21 的默认标准**（clang 16+ 的默认标准已经是 C++17 或更新）。全局 cfg 统一用的 `gnu++03` 大概率是够用的，但由于 leela_r 不在该 cfg 曾经真实编译过的名单里（跟 deepsjeng_r 一样），**这一点没有被实际编译验证过，必须在真正生成/编译时把它当第一个要检查的编译错误来源**。

### 2.3 编译参数

| 来源 | 取值 |
|---|---|
| `object.pm: $bench_flags` | `-I. -DSPEC_AUTO_SUPPRESS_OPENMP` —— `-I.` 是为了让 `#include <boost/...>` 从源码目录本身解析（现有生成器约定本来就会把源码目录加进 include path，这条应该是自动满足的，但明确写出来确保不会被漏掉）。 |
| `Makefile.defaults: CPUFLAGS` | `-DSPEC -DNDEBUG`（全 benchmark 通用）|
| 全局 cfg: `EXTRA_PORTABILITY` | `-DSPEC_LP64`（64-bit，leela_r 没有专属 `PORTABILITY` 覆盖）|
| 全局 cfg: `CXX` | 同上，`-std=gnu++03`（**必须验证，见 2.2 的 `std::auto_ptr` 约束**）|
| `SMALL_MEMORY`/`BIG_MEMORY` | 不需要——那是 deepsjeng_r 专属的硬约束，leela_r 源码里没有类似 `#error` 门禁。 |
| 链接 | 没有 `pthread`/`#pragma omp`/`omp.h` 引用，`-lm` 应该够用。 |

### 2.4 运行方式（workload / argv / 数据依赖）

跟 deepsjeng_r 同一种模式——每个 workload 尺寸是一个自包含的 `.sgf`（Smart Game Format，围棋棋谱）文件，没有 `data/all/` 共享数据：

| workload | 输入文件 |
|---|---|
| test | `data/test/input/test.sgf` |
| train | `data/train/input/train.sgf` |
| refrate | `data/refrate/input/ref.sgf` |

`object.pm::invoke()` 同款模式：匹配 `(.*)\.sgf$`，`args: ["$name.sgf"]`。同样只需要生成时把相对文件名换成绝对路径。

### 2.5 可直接转 build_manifest 的字段草稿

```jsonc
{
  "version": 1,
  "name": "leela_r",
  "sources": [ /* sources/Leela.cpp（entry）+ 20 个其余 .cpp，逐字节复制，不拼接 */ ],
  "include_dirs": ["sources"],          // 注意：不是 ["sources", "sources/boost"] ——
                                        // boost/ 的相对 include 路径要求父目录在
                                        // include path 上，不需要（也不应该）额外把
                                        // boost/ 本身单独列一条 include_dir
  "defines": ["SPEC", "NDEBUG", "SPEC_LP64", "SPEC_AUTO_SUPPRESS_OPENMP"],
  "compile_flags": ["-I."],              // 对应 object.pm 的 $bench_flags，语义上
                                        // 和 include_dirs 里的 "sources" 重复，
                                        // 保留是为了跟 object.pm 原文一一对应，
                                        // 实际生成时两者只需要生效一次
  "link_flags": ["-lm"],
  "cxx_standard": "gnu++03",            // ⚠️ 必须验证：源码用了 std::auto_ptr，
                                        // C++17+ 下会编译报错，这是继承自全局 cfg
                                        // 的猜测值，不是已验证过的已知可行值
  "workloads": {
    "test":    { "argv": ["kernel_leela_r", "{abs}/test.sgf"] },
    "refrate": { "argv": ["kernel_leela_r", "{abs}/ref.sgf"] }
  },
  "no_rundir_needed": true,
  "header_tree_note": "src/boost/ 是嵌套目录的 header-only vendor 子树（412 文件），\
必须整体子树复制保留相对路径，不能走现有 gen_spec_kernels.py/gen_spec_multitu.py \
那种\"所有 .h 拉平到一个目录\"的头文件收集方式——否则同名文件会互相覆盖，且 \
#include <boost/x/y.h> 这类带路径的 include 会解析失败。"
}
```

---

## 3. 两者与已有 namd_r 调研的差异小结

| | namd_r | deepsjeng_r | leela_r |
|---|---:|---:|---:|
| `.cpp`/`.C` 源文件数 | 15 | 20 | 21 |
| 目录结构 | 全平铺 | 全平铺 | **`src/` 平铺 + 嵌套 `boost/` 子树（412 文件）** |
| 头文件拉平是否安全 | 是 | 是 | **否——boost/ 必须整体子树复制** |
| 输入数据结构 | 共享 `data/all/input/` + 各 workload 专属 argv 文件 | 每个 workload 自包含（无共享数据）| 每个 workload 自包含（无共享数据）|
| rundir/chdir 需求 | 不需要 | 不需要 | 不需要 |
| 必需的 benchmark 专属宏 | `NAMD_DISABLE_SSE`（x86_64 上有实际语义）| `SMALL_MEMORY`（**不给会直接 `#error`**）| 无 |
| C++ 标准版本证据强度 | 较强（cfg 历史记录里真实编译过，虽然编译器路径本身是错的）| 弱（未曾被这套 cfg 真实编译过，标准版本继承自全局默认）| **弱，且有已知的强约束**（`std::auto_ptr` 在 C++17 下编译期报错，必须验证 `gnu++03` 或其他 pre-C++17 标准确实可行）|

**下一步建议（本次不做）**：三个 benchmark 里，deepsjeng_r 的结构最简单（20 文件全平铺、无特殊标准依赖之外的强约束、无嵌套头文件树），如果要按"先易后难"顺序把 `gen_spec_multitu.py` 的 `BENCHMARKS` 表扩展到 namd_r 之外，deepsjeng_r 应该排在 leela_r 之前——leela_r 需要先解决"嵌套目录头文件树怎么在生成器里保留相对结构"这个当前 `gen_spec_multitu.py`/`gen_spec_kernels.py` 都还没有的能力，属于比"新增一个 BENCHMARKS 条目"更大的一块生成器改造。
