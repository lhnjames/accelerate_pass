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

## 4. Synthetic shim generators — done

`scripts/gen_spec_kernels.py`, `gen_tsvc_kernels.py`, `gen_cbench_kernels.py`
all previously hand-wrapped non-PolyBench source in a fake PolyBench
`main()` (`WRAPPER_TEMPLATE` / `MAIN_TEMPLATE`) that included
`<polybench.h>` and called its macros purely to satisfy the old harness's
requirements. All three are now simplified: no header, no macros, no
manufactured checksum. Each just calls the renamed entry point and lets it
print/return however it naturally would; that real output goes straight to
the process's own stdout, captured directly and process-isolated by
`subprocess.run(capture_output=True)`.

- **SPEC** (`gen_spec_kernels.py`): dropped the old `#include <polybench.h>`,
  `POLYBENCH_DUMP_*` checksum wrapper, and the stdout-to-fixed-tmp-file
  redirect (`dup`/`dup2`) that only existed to keep the kernel's real
  output from colliding with `polybench_print_instruments`' own print --
  unnecessary once timing is external. mcf_r's real solve output
  (`objective value: ...`) now flows straight through. Regenerated and
  validated: mode auto-detects "numeric" (previously silently degraded to
  a no-op "hash of two empty strings" because the checksum print was dead
  code without the old macro defined).
- **TSVC** (`gen_tsvc_kernels.py`): every TSVC loop function already
  computes and *returns* its own checksum (`real_t`) as part of the
  upstream TSVC convention -- the wrapper now just `printf`s it directly,
  no macros needed at all. Regenerated (151 kernels) and validated across
  s000/s1111/s162/s151.
- **CBench** (`gen_cbench_kernels.py`): most cBench programs (dijkstra,
  bzip2 -c, adpcm-c/d, crc32, sha, qsort1, patricia, ...) already write
  their real product straight to stdout -- nothing extra needed. A handful
  (tiff2bw/tiff2dither/tiff2median/tiff2rgba, consumer-jpeg-c) take an
  explicit output-*file* argv token and write there instead; for those the
  wrapper reads that file back and streams it onto its own stdout right
  after the call, so the harness only ever looks in one place regardless
  of which convention the underlying program uses. That path is
  `getpid()`-namespaced (`{kname}_out_%d.tmp`) so a reference run and a
  candidate run can never collide on the same file -- resolving the exact
  race the file-based approach was originally flagged for. Regenerated (19
  of ~25 kernels; the rest fail on pre-existing, unrelated unity-build
  symbol collisions from concatenating multiple programs' `.c` files, not
  from anything in this pass) and validated on both a stdout-based kernel
  (bzip2_encode, dijkstra) and a file-output-based one (tiff2bw).

## 5. Bugs this surfaced along the way

- **`extract_numbers()` word-boundary bug**: a bare `\d+`-style regex also
  matches digits embedded in an identifier printed as diagnostic text --
  e.g. TSVC's `initialise_arrays()` prints the loop's own name ("s1111")
  before the checksum, and a naive scan extracted a spurious `1111.0` out
  of it. A single-character lookbehind isn't enough to prevent this (it
  still lets a match start mid-digit-run, since the preceding character
  there is another digit, not a letter). Fixed by tokenizing into
  "identifier" or "number" alternatives (identifier tried first), so the
  scanner's position jumps past the whole identifier in one match instead
  of ever considering a start position inside it.
- **Lossy stdout capture**: `_run_capture` originally used
  `subprocess.run(..., text=True, errors="replace")`, which is fine for
  text output but corrupts binary output (CBench's compressed/image/audio
  bytes) -- two genuinely different binary outputs can both contain
  invalid UTF-8 sequences that collapse to the same replacement character,
  making the `hash` tier falsely see them as equal. Fixed by capturing raw
  bytes and only decoding (losslessly, via `latin1`) when the `numeric`
  tier needs a `str` to regex-scan; the `hash` tier hashes the raw bytes
  directly.
- **`compile_c()`'s `-std=c99` vs. the CBench generator's own `-std=gnu99`
  test-compile**: some CBench sources use BSD/POSIX typedefs (e.g.
  libtiff's `u_long`) that strict c99 hides, so a kernel the generator
  accepted into the manifest could still fail when actually built through
  the harness. `gnu99` is a strict superset of `c99`, so switching
  `src/build_utils.py::compile_c` to match is backward compatible with
  every kernel that already worked.
