# 508.namd_r (C++) 构建计划——多 TU manifest 设计

**日期**: 2026-07-21
**性质**: 只读调研 + 设计文档。没有修改 `scripts/gen_spec_kernels.py`、没有跑任何计时、没有改远程任何文件、没有提交 git。全程涉及的任何本地验证性编译均使用 pinned 的 LLVM 21（`clang-21`/`clang++-21`/`opt-21`/`llc-21`），未使用 clang-11/17。
**调研范围**: SSH 到 `ubuntu@132.145.22.86`，只读读取 `/home/hanning/spec2017/benchspec/CPU/508.namd_r/` 下的 `src/`、`data/`、`Spec/object.pm`，以及 `/home/hanning/spec2017/benchspec/Makefile.defaults` 和 `/home/hanning/spec2017/config/comet-clang.cfg.2026-07-08T192459`（这台机器上已有的、曾经真实用于 mcf_r/lbm_r/nab_r/imagick_r/xz_r 五个 benchmark 的 SPEC 配置文件，其 `opthash`/`exehash` 记录证明它是真实跑通过的配置，不是猜的）。

**没有跑 `specmake -n`**：`specmake` 本质就是 SPEC 自带的 GNU make，本身不是一个能独立给出"这个 benchmark 真实编译命令"的 dry-run 工具——要让它有意义地跑，必须先用 `runcpu --action build` 之类的命令让 SPEC 官方 harness 在 `benchspec/CPU/508.namd_r/run/` 下实际生成一份构建目录和 Makefile，这本身是一次写操作，与"只读、不改远程"的约束冲突。改为直接读取 `Spec/object.pm`（每个 benchmark 自己声明的 `@sources`/`$bench_cxxflags`/`invoke()`）+ `Makefile.defaults`（`CPUFLAGS`/`PORTABILITY` 等全局变量的实际取值）+ 上述历史 cfg 文件（`CXX`/`EXTRA_PORTABILITY`/`OPTIMIZE` 的真实取值），三者组合起来是完全权威的信息源，不比跑一次 `specmake -n` 信息量少。

---

## 1. namd_r 的真实构建信息

### 1.1 源文件列表（`Spec/object.pm` 的 `@sources`，与 `src/` 下实际 `.C` 文件一一对应，共 15 个）

```
Compute.C  ComputeList.C  ComputeNonbondedFEP.C  ComputeNonbondedLES.C
ComputeNonbondedPProf.C  ComputeNonbondedStd.C  ComputeNonbondedUtil.C
LJTable.C  Molecule.C  Patch.C  PatchList.C  ResultSet.C
SimParameters.C  erf.C  spec_namd.C
```

对应 23 个头文件（`.h`），都在 `src/` 下，无子目录，规模比 xz_r/imagick_r/perlbench_r/gcc_r 小得多，是目前唯一确认可行的 C++ 候选。

### 1.2 入口

`spec_namd.C` 定义 `int main(int argc, char **argv)`。语言标记：`object.pm` 里 `$benchlang = 'CXX'`（纯 C++，无 C/Fortran 混合，比 wrf_r/cam4_r/pop2_s 这类混合语言 benchmark 简单）。

### 1.3 编译参数（来自 `object.pm` + `Makefile.defaults` + 历史 cfg 三者）

| 来源 | 取值 |
|---|---|
| `object.pm: $bench_cxxflags` | `-DNAMD_DISABLE_SSE -DSPEC_AUTO_SUPPRESS_OPENMP` |
| `Makefile.defaults: CPUFLAGS`（全 benchmark 通用，`gen_spec_kernels.py` 已经在用同一约定）| `-DSPEC -DNDEBUG` |
| 历史 cfg: `EXTRA_PORTABILITY`（64-bit 分支）| `-DSPEC_LP64` |
| 历史 cfg: `CXX` | `clang-11++ -std=gnu++03` —— **注意**：`clang-11++` 这个可执行文件名本身就不存在（应为 `clang++-11`），这正是本次会话之前在 COMET `configs/config.yaml` 里发现的同一个历史性拼写错误的源头：这台机器上 SPEC 自己的示例配置文件本来就抄错了，COMET 的 `clang_cxx_path` 大概率是照抄这份 cfg 抄错的。**编译器名字要修，但 `-std=gnu++03` 这个标准版本号本身是真实且必须保留的**（NAMD 这份 2012 年代码大概率用了一些在 C++11+ 下会报错/警告变严格的写法，实测前必须验证）。|
| 历史 cfg: `OPTIMIZE`（baseline）| `-g -O3` —— COMET 自己的 `compile_c` 已经统一加 `-O3`，`-g` 可选，不影响优化结果，可以不带。|
| SSE 检测 | `src/ComputeNonbondedBase.h` 有 `#if defined(__SSE2__) && !defined(NAMD_DISABLE_SSE)` 包住的 `<emmintrin.h>`（x86 SSE2 intrinsics）路径。**这台远程机器是 aarch64**，`__SSE2__` 永远不会被定义，这条宏在远程上其实是死代码保护；但本地这台 sandbox 是 x86_64（已确认 `clang-21`/`clang++-21` 都是 x86_64 build），如果将来在本地验证编译，`-DNAMD_DISABLE_SSE` 是必须带的，否则会误入 SSE2 intrinsics 分支。|
| 链接 | 源码里没有任何 `pthread`/`omp.h`/`#pragma omp` 引用（`SPEC_AUTO_SUPPRESS_OPENMP` 只是防御性预留，这份源码实际没用到 OpenMP），链接只需要 `-lm`，跟现有 `compile_c` 的默认行为一致，不需要额外链接库。|

**结论**：真实编译命令大致等价于（简化写法，实际以 manifest 为准）：

```
clang++-21 -O3 -std=gnu++03 -DSPEC -DNDEBUG -DSPEC_LP64 -DNAMD_DISABLE_SSE -DSPEC_AUTO_SUPPRESS_OPENMP \
    Compute.C ComputeList.C ComputeNonbondedFEP.C ComputeNonbondedLES.C ComputeNonbondedPProf.C \
    ComputeNonbondedStd.C ComputeNonbondedUtil.C LJTable.C Molecule.C Patch.C PatchList.C \
    ResultSet.C SimParameters.C erf.C spec_namd.C \
    -o namd_r -lm
```

### 1.4 运行方式（`invoke()` + `data/`）

`Spec/object.pm::invoke()` 的实际逻辑：读取一个跟 workload 同名的 `<name>.in` 文件（这里 `namd.in`），把它的**文本内容**整个按空白切分成 argv（不是把这个文件喂给程序当输入）。真正的输入数据是 argv 里 `--input` 指向的文件。

- `data/test/input/namd.in` 内容：`--input apoa1.input --iterations 1 --output apoa1.test.output`（快速冒烟用）
- `data/refrate/input/namd.in` 内容：`--input apoa1.input --output apoa1.ref.output --iterations 65`（官方 reference 规模）
- 真正的分子/模拟数据文件 `apoa1.input`（8.09 MB）不在 `test/`/`refrate/` 目录下，而在 `data/all/input/apoa1.input`（SPEC 约定：跨 workload 规模共享的文件放在 `all/`，每个规模目录只放该规模专属的文件，这里只有 argv 字符串）。

`spec_namd.C::main()` 对 `--input`/`--output`/`--standard` 的值直接 `fopen(value, "r"/"w")`，**没有内部再拼接目录名**——不像 nab_r 需要 `chdir()` 才能让程序自己算出的相对路径生效。这意味着 namd_r 比 nab_r 简单：**只需要在生成 wrapper 时把 `--input`/`--output` 的值换成绝对路径**，完全不需要 nab_r 那套 `rundir` + `chdir()` 机制。

程序自身的 stdout 会打印形如 `iteration 3: 1 0 0 1` 这样的数值行（来自 `RUN_AND_CHECKSUM`/`SET_MODE` 宏），加上末尾的 checksum 比较信息——`src/correctness.py` 的 `numeric` 档位应该可以直接工作，不需要额外适配。

---

## 2. 为什么现有架构不能直接套用 nab_r 的 unity-build 做法

nab_r 是 11 个纯 C 文件、程序化风格代码；namd_r 是 15 个真实面向对象 C++ 文件（`Molecule`/`Patch`/`ComputeList`/`SimParameters` 等类，彼此通过头文件互相引用）。C++ 下把非入口文件的**内容**整个文本拼接进一个共享 `polybench.c`/`polybench.cpp`（现有 `gen_spec_kernels.py::gen_one()` 对 C benchmark 的做法）风险比 C 高得多：

- 匿名命名空间 / 文件作用域 `static` 符号：C++ 里这类用法比 C 更常见（每个 `.C` 文件几乎都有一些 file-local helper），强行拼进同一个 TU 会产生比这次 nab_r 遇到的还多的同名冲突。
- 宏污染跨文件泄漏：一个文件里 `#define`（不 `#undef`）的宏会漏进物理上排在它后面的所有其他文件的编译结果里，C++ 模板/内联函数对宏定义更敏感。
- 相同的头文件在拼接后的单一 TU 里如果被不同源文件以不同的宏上下文 `#include` 两次（这次 nab_r 的 `engine.ih`/`regex2.h` 那类问题），C++ 的类定义/模板实例化比 C 的普通声明更容易触发真正的 ODR 冲突而不是"重复但无害"。
- 更根本的问题：C++ 下"拼成一个文件"完全没有必要——`compile_c()`（`src/build_utils.py`）本来就支持一次调用传入多个源文件（`clang++ a.C b.C c.C -o out`），clang 会对每个文件分别起一个 `-cc1` 编译成独立的目标文件再链接，这就是原生的、经过验证的多 TU 编译，**不需要任何拼接就能正确工作**。真正挡路的不是 `compile_c()`，而是 `optimize.py`/`src/hotspot.py`/`tune_source.py` 里"一个 kernel 的源码 = driver 文本 + utils 文本，只有这两个字符串"这个贯穿全架构的假设（热点探测的调用图只在这两段文本里找函数、`rewrite_source` 定位/替换目标函数体也只在这两段文本里找）。

---

## 3. Multi-TU Build Manifest：JSON Schema 设计

新增一个每-benchmark 的 JSON 文件（例如 `SPEC_shim_root/namd_r/SPEC_shim/build_manifest.json`），描述"这个 kernel 由哪些独立编译的翻译单元组成"，取代"driver.c + 一个拼接的 polybench.c"这个写死的二元假设。

```jsonc
{
  "schema_version": 1,
  "kernel_name": "kernel_namd_r",
  "language": "cxx",                     // "c" | "cxx" -- 决定 select_compiler() 走哪支
  "compiler_std": "gnu++03",              // 显式传给 compile_c() 的 extra_flags，
                                          // 不使用 clang++ 默认标准版本
  "entry": {
    "file": "spec_namd.C",
    "role": "entry"                       // 含 main()，重命名规则同现有 rename_entry_all()
  },
  "translation_units": [                  // 除 entry 外，每个原始 .C 文件保持独立，
    {"file": "Compute.C"},                // 不做任何文本拼接——每个条目对应一次独立的
    {"file": "ComputeList.C"},            // clang++ -c 调用（或在同一条 compile_c() 命令行
    {"file": "ComputeNonbondedFEP.C"},    // 里作为独立的位置参数，效果相同，见第 4 节）
    {"file": "ComputeNonbondedLES.C"},
    {"file": "ComputeNonbondedPProf.C"},
    {"file": "ComputeNonbondedStd.C"},
    {"file": "ComputeNonbondedUtil.C"},
    {"file": "LJTable.C"},
    {"file": "Molecule.C"},
    {"file": "Patch.C"},
    {"file": "PatchList.C"},
    {"file": "ResultSet.C"},
    {"file": "SimParameters.C"},
    {"file": "erf.C"}
  ],
  "headers": "*.h",                       // 平铺复制到 kernels/<name>/，无需 include-guard
                                          // 修补（跟 nab_r 不同，这些头本来就都带 guard）
  "defines": [                            // 顺序无关，最终拼成 -D<name> / -D<name>=<val>
    "SPEC", "NDEBUG",                    // 全 benchmark 通用（现有 SPEC_DEFINES_H 约定）
    "SPEC_LP64",                          // 来自 Makefile.defaults 的 EXTRA_PORTABILITY（64-bit）
    "NAMD_DISABLE_SSE",                   // benchmark 专属：本地 x86_64 sandbox 必须带
                                          // （否则会误入 SSE2 intrinsics 分支），远程 aarch64
                                          // 上是 no-op 但不带也无害，统一带上更安全
    "SPEC_AUTO_SUPPRESS_OPENMP"           // benchmark 专属，源码实际未用 OpenMP，防御性保留
  ],
  "link_flags": ["-lm"],                  // 无 pthread/omp，跟现有 compile_c 默认值一致
  "argv": {                               // 对应 invoke() 解析出的 workload 参数，
    "workload": "refrate",                // 但把相对路径换成生成时刻解析好的绝对路径
    "template": [
      "kernel_namd_r",
      "--input", "{DATA_ALL_INPUT}/apoa1.input",
      "--output", "{RUN_TMP}/apoa1.ref.output",
      "--iterations", "65"
    ]
  },
  "no_rundir_needed": true,                // 与 nab_r 的对比说明：spec_namd.C 直接 fopen(argv值)，
                                          // 不像 nab_r 内部自己拼 "<argv1>/<argv1>.pdb"，
                                          // 不需要 chdir()，只需生成时把路径换成绝对路径
  "correctness_hint": "numeric"            // stdout 含 "iteration N: E F M savePairlists" 之类
                                          // 数值行，src/correctness.py 的 numeric 档应可直接用
}
```

**为什么不用 nab_r 那套 `text_fixups`**：`text_fixups` 是"仍然拼接，但对拼接前的某个文件打一个针对性文本补丁"这个模式——前提是还在做拼接。这份 manifest 的设计思路是**从根源上不拼接**，所以不需要 `text_fixups` 这个概念；如果将来 namd_r 真编译时发现某个文件之间有需要修的东西（比如某个头文件缺 include guard），可以在 manifest 里加一个可选的 `header_fixups` 字段，语义上类比但作用于头文件而非拼接后的整体源码。

---

## 4. 对现有代码的最小改造点

### 4.1 `src/build_utils.py::compile_c` —— **不需要改**

已经原生支持"多个源文件位置参数 = 多个独立 TU"（`cmd = [compiler, ...] + list(sources) + ["-o", out, "-lm"]`，clang 对每个文件单独起 `-cc1`）。manifest 的 `translation_units` 列表 + `entry.file` 拼起来就是 `sources` 参数，直接可用。`compiler_std` 字段通过已有的 `extra_flags` 参数（`compile_binary(..., extra_flags=["-std=gnu++03"])`）传入，不需要改 `compile_c` 的签名。

### 4.2 `scripts/gen_spec_kernels.py` —— 需要新增一个生成分支

当前 `gen_one()` 硬编码"入口文件 + 所有非入口文件的内容拼进一个 polybench.c"。需要新增一个 per-benchmark 配置开关（例如 `"multi_tu": True`），命中时：
  - 不做任何文本拼接；把 `translation_units` 里列出的每个文件**原样复制**（不改名、不改内容）到 `kernels/<name>/`。
  - 生成 manifest JSON 文件（本文档第 3 节的 schema），而不是生成一个"utils/polybench.c"。
  - 入口文件仍按现有 `rename_entry_all()` 逻辑重命名 `main` → `kernel_<name>`，写入 wrapper。

### 4.3 `optimize.py`/`src/hotspot.py`/`tune_source.py` —— 需要把"两段文本"泛化成"N 个文件"

这是工作量最大、且**这次任务范围明确不做**的部分，只在这里记录改造范围供下次规划：

- `collect_all_evidence()`（`optimize.py`）目前假设 `ev["utils"]` 是一个目录、`ev["utils"]/polybench.c` 是单一文件；需要改成 `ev["translation_units"]`（文件路径列表），后续所有读 `ev["utils"]`/utils 文本的地方都要改成"在 N 个文件里找"。
- `src/hotspot.py::_build_and_score()` 目前签名是 `(kernel_name, driver_text, utils_text, max_hops)`——只接受两段字符串；需要泛化成接受一个 `{file_path: source_text}` 字典，调用图遍历、函数体提取都要在这个字典上找，而不是只在两段文本里找。
- `tune_source.py::extract_kernel_function()` 目前是"先在 driver_text 里找，找不到再在 utils_text 里找"的二选一逻辑；需要泛化成"遍历 manifest 里列出的每个文件，返回命中的那个文件路径 + 函数体 span"，因为 `rewrite_source` 需要知道具体改写哪个物理文件才能正确写回。
- `_splice_multi_spans`（`optimize.py`，本次会话新增的多函数联合改写机制）目前假设所有目标函数都在**同一段共享文本**（utils_text）里，需要泛化成"每个目标函数各自记录自己所在的文件，分别 splice 各自的文件"。

### 4.4 正确性验证 / 计时 —— 预期不需要改

`src/correctness.py`（numeric/hash/exit_only 三档）和 `src/build_utils.py::run_timing`（外部墙钟计时）都是对**编译产物二进制**操作，不关心源码是几个文件、怎么编译出来的，天然适配多 TU 构建，不需要改。

---

## 5. 建议的下一步顺序（本次不做，仅记录）

1. 先在 4.2（`gen_spec_kernels.py` 新增 `multi_tu` 分支）+ 手动指定固定的 `argv`/`defines` 上跑通"能编译、能正确运行、能通过 numeric 正确性检查"这一步——此时还不需要动 `optimize.py`/`hotspot.py`，可以先用现成的 `--graph-only`/`tune_param.py --param-only` 之类的独立入口验证"这个 kernel 能被基础设施识别、能提取 IR、能测出 baseline 时间"，把 4.3 的架构改造推迟到确认基础编译链路走通之后。
2. 4.3 的架构泛化是真正的大改动，建议先只支持"单文件热点"（即假设真正的性能热点仍然落在某一个文件里，只需要泛化"去哪个文件里找函数"这个查找逻辑，不需要一开始就支持跨文件联合改写），验证通过后再考虑是否需要对 namd_r 做跨文件的联合 rewrite_source（这次会话新增的 gap-detection 联合选点机制目前完全没有跨文件版本，如果 namd_r 的热点真的分散在多个文件的多个函数里，需要先把 gap-detection 和联合 splice 都扩展到多文件语义，工作量会显著增加）。
