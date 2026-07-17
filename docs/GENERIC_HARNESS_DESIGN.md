# Generic (non-PolyBench) timing + correctness harness

## Why

The harness (`optimize.py`, `tune_param.py`, `tune_source.py`, `src/build_utils.py`)
currently requires every kernel to be "PolyBench-shaped": include `polybench.h`,
call `polybench_start_instruments` / `polybench_stop_instruments` /
`polybench_print_instruments` for timing, and call `POLYBENCH_DUMP_START` /
`POLYBENCH_DUMP_BEGIN` / `POLYBENCH_DUMP_END` / `POLYBENCH_DUMP_FINISH` for
correctness verification.

Real upstream PolyBench-C kernels (`2mm.c`, `gemm.c`, ...) are naturally
written this way already — no problem there. But the SPEC/TSVC/CBench
"shims" are synthetic: `scripts/gen_spec_kernels.py` / `gen_tsvc_kernels.py`
/ `gen_cbench_kernels.py` hand-wrap benchmarks that have *nothing to do with
PolyBench* in a fake PolyBench dressing, purely so the existing harness code
doesn't need to change. That coupling is what we're removing, so any C
program — with no header dependency and no required macro calls — can be
optimized by this system.

Two separable concerns were bundled into `polybench.h` and need independent,
generic replacements:

1. **Timing** — currently self-reported by the binary via
   `polybench_start/stop/print_instruments`, parsed by the harness as
   "the last whitespace-separated token of stdout".
2. **Correctness verification** — currently requires the binary to print a
   specially-marked numeric dump (`POLYBENCH_DUMP_ARRAYS`) that the harness
   parses looking for `begin dump:` / `end dump:` markers.

## 1. Generic timing — pure external wall-clock

Replace `run_timing()` in `src/build_utils.py`. Instead of running the binary
and parsing a self-reported number out of its stdout, wrap the whole process
in `time.monotonic()` from the harness side:

```python
def run_timing(bin_path: str, runs: int = 5, pin_cpu: "int | None" = None) -> float:
    cmd = (["taskset", "-c", str(pin_cpu)] if pin_cpu is not None else []) + [bin_path]
    try:
        subprocess.run(cmd, capture_output=True, timeout=60)   # warmup, discarded
    except Exception:
        pass
    times = []
    for _ in range(runs):
        t0 = time.monotonic()
        try:
            res = subprocess.run(cmd, capture_output=True, timeout=60)
        except Exception:
            continue
        elapsed_ms = (time.monotonic() - t0) * 1000.0
        if res.returncode == 0:
            times.append(elapsed_ms)
    return statistics.median(times) if times else -1.0
```

Consequences:
- No `-DPOLYBENCH_TIME` needed, no `polybench_start/stop/print_instruments`
  calls needed in the target source at all.
- Slightly less precise than an internal `clock_gettime` measurement
  (process-launch overhead is now inside the measured window), but that
  overhead is systematic across baseline and candidate builds, so relative
  speedup comparisons are unaffected — this was an accepted tradeoff
  (external wrapping vs. internal timing hook) per the user's explicit
  choice.
- Backward compatible: this doesn't care what the binary prints, so it works
  identically whether or not the binary happens to also print a PolyBench
  timing line (that line just becomes irrelevant, ignored stdout noise).

## 2. Generic correctness verification — three tiers, no markers required

New module: `src/correctness.py`. Three tiers, auto-detected per kernel
(mirrors today's `_detect_polybench_mode`, generalized):

| Tier | What it checks | When selected |
|---|---|---|
| `numeric` | Extract every numeric token from the program's raw stdout+stderr (+ optional output file), compare element-wise with `epsilon` relative-error tolerance | Reference run's output contains ≥1 parseable number |
| `hash` | SHA256 of raw output bytes, exact match required | Output is deterministic (two reference runs match byte-for-byte) but not usefully numeric (e.g. text/binary data) |
| `exit_only` | Only that the process returns exit code 0 | Neither of the above applies (nondeterministic or empty output) |

Key design choice vs. the old `extract_numbers_from_dump`: **no marker
scanning at all.** The old parser looked for `begin dump:` / `end dump:`
lines and only extracted numbers between them — which is exactly what broke
for SPEC/TSVC's single-line checksum dumps (value trailing the marker with
no separator) and is inherently marker-format-fragile. The new
`extract_numbers()` just regex-scans the *entire* captured text for
float-looking tokens, unconditionally. This is a strict superset: existing
PolyBench-style dumps (which already print nothing but numbers between
BEGIN/END lines) still parse identically; SPEC/TSVC-style single-line
checksums parse correctly for the first time; and completely undecorated
program output (a CBench kernel that just does `printf("%d\n", result)`)
now works with zero source changes.

```python
# src/correctness.py — sketch (full impl in the actual patch)

FLOAT_RE = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")

def extract_numbers(text: str) -> "list[float] | str":
    """Every numeric token in `text`, in order. Returns an error string on
    NaN/Inf (always a real computation bug, never an accepted difference)."""

def compare_numeric(v1: list, v2: list, epsilon: float) -> tuple[bool, str]:
    """Same semantics as today's tune_source.compare_outputs (size check,
    all-zero suspicion check, max relative error vs epsilon), just no
    longer named/scoped as a PolyBench-only helper."""

def detect_correctness_mode(bin_path: str, output_file: "Path|None", timeout: int) -> str:
    """Run the (already-built) reference binary once or twice, pick the
    richest tier that applies: numeric > hash > exit_only."""

def check_correctness(ref_bin: str, opt_bin: str, mode: str,
                      epsilon: float, timeout: int,
                      output_file: "Path|None") -> tuple[bool, str]:
    """Run both binaries, dispatch to the comparison for `mode`."""
```

`optimize.py`'s `_correctness_check()` is simplified to: compile ref and
candidate (unchanged — still needs `utils`/extra source files for
multi-file kernels, that's a build concern, not a PolyBench concern), then
call `check_correctness()` instead of hand-rolling three PolyBench-flavored
branches inline. `_detect_polybench_mode` is replaced by
`detect_correctness_mode` (no longer needs a special
`-DPOLYBENCH_DUMP_ARRAYS` test-compile — it just runs the binary that's
already built for other purposes).

An optional per-kernel `output_file` path lets kernels that write results to
disk instead of stdout (e.g. some CBench programs) opt into `numeric`/`hash`
tiers too, instead of falling all the way back to `exit_only`.

## 3. What does *not* need to change

Real upstream PolyBench-C kernels (`2mm.c`, `gemm.c`, `correlation.c`, ...)
keep using `polybench.h`'s `POLYBENCH_2D`/`POLYBENCH_1D`/alignment macros —
those are the kernels' own array-declaration idiom, not harness
instrumentation, and are out of scope here. Their existing
`POLYBENCH_DUMP_ARRAYS`-gated `print_array()` output keeps working
unmodified under the new `numeric` tier (marker-agnostic extraction is a
superset of marker-aware extraction). **Zero source changes needed for the
~30 real PolyBench-C benchmarks.**

`find_polybench_utilities()` / `POLYBENCH_DIR_NAMES` (`src/polybench_paths.py`)
is kept as-is functionally — it's a directory-lookup convention (find a
`utilities/` folder holding extra `.c` files a multi-file kernel needs
linked), not a macro/header coupling. Not touched in this phase.

## 4. Synthetic shim generators — full rewrite (deferred to a follow-up pass)

`scripts/gen_spec_kernels.py`, `gen_tsvc_kernels.py`, `gen_cbench_kernels.py`
currently hand-wrap non-PolyBench source in a fake PolyBench `main()`
(`WRAPPER_TEMPLATE` / `MAIN_TEMPLATE`) that includes `<polybench.h>` and
calls its macros purely to satisfy the old harness's requirements. Once the
harness no longer requires that, these templates simplify to: call the
renamed entry point directly, let it print/return however it naturally
would, done. No header, no macros, no manufactured checksum wrapper needed
unless the target genuinely has no stdout output of its own (rare — kept as
an opt-in fallback, not the default).

This full regeneration (all 4 shim families, re-verified end to end) is
scoped as a separate follow-up pass once the core module lands and is
validated — today's slice is: core module implemented + wired into
`optimize.py`, validated against SPEC's `mcf_r` (a good test case since its
actual algorithm has nothing to do with PolyBench; only its current
generator-added wrapper does).
