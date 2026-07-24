#!/usr/bin/env bash
# Five deliberately difficult end-to-end LLVM 21 runs spanning cBench and
# PolyBench.  Runs sequentially so timing/profiling never overlaps locally.
set -uo pipefail

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$PROJECT_ROOT"

ROUNDS=${ROUNDS:-5}
RUNS=${RUNS:-5}
PIN_CPU=${PIN_CPU:-}
SKILLS_OFF=${SKILLS_OFF:-0}
STAMP=${COMET_RUN_STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
LOG_ROOT=${LOG_ROOT:-"logs_five_normal/$STAMP"}
mkdir -p "$LOG_ROOT"

NAMES=(
  bzip2_encode
  automotive_susan_corners
  cholesky
  lu
  seidel-2d
)
PROGRAMS=(
  CBench_shim_root/cbench-bzip2_encode/CBench_shim/kernels/bzip2_encode/bzip2_encode.c
  CBench_shim_root/cbench-automotive-susan_corners/CBench_shim/kernels/automotive_susan_corners/automotive_susan_corners.c
  PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c
  PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c
  PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c
)

TOOLS=(clang-21 clang++-21 opt-21 llc-21)
for tool in "${TOOLS[@]}"; do
  path="$PROJECT_ROOT/scripts/toolchain/$tool"
  if [[ ! -x "$path" ]]; then
    echo "ERROR: required LLVM 21 launcher is missing: $path" >&2
    exit 2
  fi
  version=$($path --version 2>&1 | head -1)
  if [[ "$version" != *"21."* && "$version" != *"version 21"* ]]; then
    echo "ERROR: $tool is not LLVM 21: $version" >&2
    exit 2
  fi
  printf '%s\t%s\n' "$tool" "$version" >> "$LOG_ROOT/toolchain.tsv"
done

for program in "${PROGRAMS[@]}"; do
  if [[ ! -f "$program" ]]; then
    echo "ERROR: benchmark source missing: $program" >&2
    exit 2
  fi
done

printf 'benchmark\tstatus\tstarted_utc\tfinished_utc\tlog\n' > "$LOG_ROOT/status.tsv"
if [[ "$SKILLS_OFF" == "1" ]]; then
  SKILLS_MODE=off
else
  SKILLS_MODE=on
fi
echo "Five-test LLVM 21 run: rounds=$ROUNDS runs=$RUNS pin_cpu=${PIN_CPU:-none} skills=$SKILLS_MODE"
echo "Logs: $LOG_ROOT"

failures=0
for i in "${!PROGRAMS[@]}"; do
  name=${NAMES[$i]}
  program=${PROGRAMS[$i]}
  log="$LOG_ROOT/$name.log"
  started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  echo "[$started] START $name"

  cmd=(python3 optimize.py --program "$program" --rounds "$ROUNDS" --runs "$RUNS")
  if [[ -n "$PIN_CPU" ]]; then
    cmd+=(--pin-cpu "$PIN_CPU")
  fi
  if [[ "$SKILLS_OFF" == "1" ]]; then
    cmd+=(--skills-off)
  fi

  if "${cmd[@]}" > "$log" 2>&1; then
    status=PASS
  else
    status=FAIL
    failures=$((failures + 1))
  fi
  finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$status" "$started" "$finished" "$log" >> "$LOG_ROOT/status.tsv"
  echo "[$finished] $status $name"
done

echo "Completed five-test run with $failures failure(s)."
exit "$failures"
