# COMET paper experiment protocol (LLVM 21)

This protocol separates compiler-parameter decision quality from source-code
rewriting, uses equal measurement budgets, and reports search variance rather
than a single lucky rollout.

> **Reporting-policy update (2026-07-24, decided by the project owner).**
> Two changes to how the final number is chosen:
>
> 1. **No rollback.** The earlier rule "confirmed < 1.0 must be rolled back to a
>    reported 1.000x" is removed. The agent LLM already judges which candidate
>    is best; we do not override it with a 1.0 floor.
> 2. **Best-observed speedup.** Across the n alternating confirmation runs, the
>    reported number is the **best (fastest) observed** paired speedup
>    (`best_speedup` = max paired ratio = min-baseline-time / min-candidate-time),
>    not the median. Rationale: execution-time noise is one-sided — interference
>    only slows a run down — so the fastest run is the least-perturbed estimate
>    of true compute time. Operationally: if even one of the n runs shows a gain,
>    that gain is taken as achievable; a candidate is reported below 1.0 only
>    when it regresses in **every** run (`n_positive == 0`).
>
> Alongside the headline best-observed number, each result also records
> `confirmed_median`, `n_positive`/`n_runs`, the IQR, and `significant_gain`
> (= median > 1.0, i.e. reliably rather than occasionally faster) for full
> transparency. **Correctness enforcement is unchanged**: a numerically wrong
> candidate is rejected during its step and never becomes the reported result,
> so reporting the best observed *speed* never publishes an incorrect result.
> The `exploratory_speedup` (search-time single-shot peak) stays a separate
> field, never conflated with the confirmed number.

## Toolchain and host controls

- Local engineering validation: LLVM 21.1.5 launchers in
  `scripts/toolchain/`; no LLVM 11/17 fallback.
- Final SPEC measurements: LLVM 21.1.8 system packages on
  `ubuntu@132.145.22.86`.
- One benchmark process at a time. Pin it to one physical core and leave its
  SMT sibling unused. Record CPU model, microcode, governor, kernel, tool
  versions, process affinity, ambient load, and command line with every run.
- Warm up once, then alternate O3/candidate measurements. Flush the software
  cache buffer consistently. Report raw samples, median paired speedup, IQR,
  coefficient of variation, and bootstrap 95% confidence interval.
- Reject results that fail LARGE/ref-data correctness. Binary outputs use exact
  SHA-256; textual numeric outputs use the declared tolerance.

## Initial difficult five

| Suite | Benchmark | Stress case |
|---|---|---|
| cBench | bzip2_encode | multi-source compression and binary correctness |
| cBench | automotive_susan_corners | image I/O and large hotspot function |
| PolyBench | cholesky | triangular dependencies and numerical stability |
| PolyBench | lu | loop-carried dependencies and cache behavior |
| PolyBench | seidel-2d | inherently sequential stencil dependence |

The normal-search batch is run by `scripts/run_five_normal.sh`, sequentially.

Before every forced parameter-debugging action, COMET now runs a dedicated
`pass-analysis` skill. It audits every pass observed in the LLVM 21 O3
pipeline, records its purpose and FIRED/no-op/skipped status, relates it to the
hotspot/runtime counters and prior measurements, and emits only validated
numeric options from `opt-21 --help-hidden`. The subsequent tuning action is
restricted to those audited options; the normalized audit is saved in each
`*_agent_results.json`.

## Independent search repetitions

For each benchmark and search method, run at least five independent searches
with seeds `101, 211, 307, 401, 503`. A search repetition includes all agent
decisions and candidate evaluations; final timing repetitions are not a
substitute for independent searches. Temperature, model identifier, skill
versions, compiler option discovery hash, and measurement budget must be
recorded per repetition.

Primary search-level statistics:

- distribution of final confirmed speedup;
- success probability (`confirmed_speedup > 1.01` and correctness passes);
- median evaluations-to-best and wall time-to-best;
- between-rollout variance and bootstrap 95% confidence interval.

## Equal-budget non-LLM baselines

Run these in `--param-only` mode over the same LLVM 21 numeric options and
candidate bounds discovered from `opt-21 --help-hidden`:

1. COMET skill-guided parameter decisions;
2. seeded uniform random search;
3. Bayesian optimization using a Gaussian-process surrogate and expected
   improvement.

Each method receives the same number of successful compile-and-measure
evaluations. Failed compilations consume one evaluation, as they also consume
real search budget. All methods share the same initial O3 sample, correctness
gate, timing policy, and final confirmation. Source rewriting is excluded from
this comparison because random/Bayesian search has no equivalent source action.

The executable harness is `scripts/run_search_baseline.py`; it consumes one
shared JSON axis catalog (the checked-in smoke catalog is
`configs/baseline_axes_llvm21.json`), verifies clang/clang++/opt/llc 21, builds
each candidate, applies the correctness gate, and writes raw trial outcomes.
`scripts/summarize_search_results.py` reports failed-trial counts, median best
speedup, IQR, success probability, and a seeded bootstrap interval.

## Component ablations

Use the full action space and identical maximum steps. Evaluate:

- full system;
- no rich remarks (retain basic missed-remark text);
- no rollback (still record catastrophic regression);
- no meta-planning (decision skill chooses each action directly);
- no failure reflection;
- no persistent strategy memory;
- no hotspot selection (rewrite the harness entry only);
- skills removed, using the same generic model with task context only.

Change one component at a time. Use the same five seeds and paired benchmark
order. The primary effect size is the paired difference in confirmed speedup;
also report correctness failure rate, invalid compile rate, token use, calls,
and measurements.

## SPEC scope

The actual source inventory on the remote host defines language eligibility.
Pure C and pure/mixed C+C++ programs are in scope. Programs requiring Fortran
are excluded, even if an old benchmark list mislabeled them as C++.

- Pure C++ / C+C++ candidates: `namd_r`, `parest_r`, `povray_r`, `omnetpp_r`,
  `xalancbmk_r`, `blender_r`, `deepsjeng_r`, `leela_r`.
- `cactuBSSN_r` includes C, C++, and Fortran and is excluded unless a separately
  approved flang-21 extension is implemented.
- `wrf_r`, `cam4_r`, `pop2_s`, and `exchange2_r` require Fortran and are excluded.
- Remaining pure C programs and `specrand_ir` remain in scope.

All SPEC claims must use the reference workload and the remote isolated CPU;
test workloads are only integration smoke tests.
