# SPEC CPU2017 integration status

Mirrors `docs/POLYBENCH_RUN_STATUS.md`'s role for TSVC/cBench. Generator:
`scripts/gen_spec_kernels.py`. Output tree: `SPEC_shim_root/<kname>/SPEC_shim/`
(same `kernels/` + `utilities/` shape as `TSVC_shim` / `CBench_shim`, so
`find_polybench_utilities` resolves it unmodified — `SPEC_shim` was added to
`POLYBENCH_DIR_NAMES` in `src/polybench_paths.py`).

The harness (`optimize.py`) only ever compiles exactly two translation units
per kernel ([driver.c, polybench.c]) with `-I{utilities} -I{source_dir}`.
For a SPEC benchmark this means: rename its real `main()` to `kernel_<name>`,
flatten every header it uses into one directory (copied into both
`kernels/<name>/` and `utilities/`), and concatenate every *other* source
file into a benchmark-private `utilities/polybench.c` after the real
PolyBench runtime — a unity build of everything except the driver, same
trick `gen_cbench_kernels.py` uses. `-DSPEC -DNDEBUG` (SPEC's own
`Makefile.defaults: CPUFLAGS`) are baked in via a `spec_defines.h` prelude
in both TUs instead of touching `compile_binary()`'s fixed define list.

Correctness check follows the cBench convention: capture the driver's real
stdout (redirected around the call so it doesn't collide with
`polybench_print_instruments`'s own stdout print), checksum it, dump via
`POLYBENCH_DUMP_*`. Workload = SPEC's `test` size (fastest, meant for
functional checks, not `refrate`/`refspeed`) — appropriate for an
iterative LLM tuning loop doing many compiles per kernel.

## Done (compiled + ran clean with clang-11, verified via optimize.py --graph-only)

| kernel  | benchmark  | files | notes |
|---------|-----------|-------|-------|
| mcf_r   | 505.mcf_r | 11    | network simplex solver, argv = [input file, ""] |
| lbm_r   | 519.lbm_r | 2     | lattice-Boltzmann fluid dynamics, argv = [20 steps, result path, 0, 1, obstacle path] |

## Good next candidates (not yet generated)

- **999.specrand_ir / 998.specrand_is / 997.specrand_fr / 996.specrand_fs**
  — trivial (`main.c` + one `specrand-common/` helper), C, essentially a
  PRNG micro-kernel. Should be the easiest possible next addition.
- **544.nab_r** (19 files, single `main`) — blocked on one detail: its
  `argv[1]` is a *directory name* resolved relative to CWD (SPEC copies
  `data/test/input/hkrdenq/` into the run dir), not an absolute path passed
  straight through like mcf/lbm. Fix is small: either `chdir()` to
  `data/test/input/` before calling `kernel_nab_r`, or check exactly how
  `nabmd.c` builds file paths from `argv[1]` and pass an absolute prefix
  directly. Didn't chase it down yet.
- **525.x264_r** (video encoder, ~90 files) — pure C, no dual-entry harness
  issue, but bigger unity-build surface (more chance of a symbol collision
  or missing `-D` than mcf/lbm). Worth trying next after nab.

## Deferred, larger lift

- **557.xz_r** — pure C but has *two* `main()`s in different files
  (`spec.c`'s harness dispatcher and `pxz.c`'s real CLI main); SPEC's
  `spec.c`/`SpecMain()` pattern is also used by 502.gcc_r, 500.perlbench_r,
  538.imagick_r, 600/602/638/657 (`_s` twins). Needs one extra step: figure
  out which of the two `main`s is the real active one under
  `-DSPEC_XZ`/equivalent and rename only that one, or special-case the
  dispatcher. Same pattern would then reuse for gcc_r/perlbench_r/imagick_r.
- **500.perlbench_r, 502.gcc_r** — same SpecMain harness issue plus very
  large file counts (perlbench embeds a full Perl interpreter; gcc_r is
  GCC's own source). Higher risk of the unity-build trick hitting a real
  multiple-definition or macro-redefinition clash. Worth attempting only
  after xz_r validates the SpecMain-rename approach on a smaller target.
- **538.imagick_r** — same SpecMain pattern, large (ImageMagick core).

## Out of scope for this harness (not attempted)

- **Fortran-language benchmarks**: 503.bwaves, 521.wrf, 527.cam4,
  549.fotonik3d, 554.roms, 548.exchange2, 628.pop2 (+ `_s` twins),
  507.cactuBSSN (mixed F). The harness is clang/LLVM-C-pass-remark driven
  end to end (`-mllvm` flags, `clang -Rpass=...` remarks, `#pragma clang
  loop` hints) — none of that applies to a Fortran compiler.
- **C++ benchmarks**: 508.namd, 510.parest, 511.povray, 520.omnetpp,
  523.xalancbmk, 526.blender, 531.deepsjeng, 541.leela (+ `_s` twins).
  Not fundamentally impossible (`clang++` also emits `-Rpass=` remarks and
  supports the same `-mllvm` flags), but `tune_source.py`'s source-rewrite
  channel (`extract_kernel_function`, prompt templates) is written against
  C-shaped kernels; templates/classes/overloads would need real work to
  parse and rewrite safely. Treat as a separate, later phase.

## Not covered

`_s` (speed/OpenMP) twins of every `_r` (rate) benchmark share source with
their `_r` counterpart — once a `_r` kernel is generated, the `_s` twin is
a near-duplicate manifest entry (same source, `-DSPEC_OPENMP` for
intspeed/fpspeed) and not separately valuable for the optimizer to explore;
skip generating both unless the paper specifically wants a speed-vs-rate
comparison point.
