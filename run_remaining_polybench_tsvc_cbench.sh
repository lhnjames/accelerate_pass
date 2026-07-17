#!/bin/bash
# Sequentially run, in a single process, one kernel at a time (no concurrency):
#   Stage 1: the 12 PolyBench kernels NOT yet completed by the 2026-07-07
#            run_polybench.sh pass (ludcmp was mid-run and never finished;
#            lu..seidel-2d never started -- their logs/*.log still held
#            stale 2026-06-24 data, archived to logs/archive_pre_2026-07-10/)
#   Stage 2: all 151 TSVC kernels  (TSVC_shim/manifest.txt)
#   Stage 3: all 19  cBench kernels (CBench_shim_root/manifest.txt)
#
# The 18 PolyBench kernels already completed 2026-07-07..07-10 (correlation
# .. gramschmidt) are NOT re-run and their logs/*.log are NOT touched --
# this script only ever writes to the kernel-name log files listed below.
#
# All stages use --rounds 5 --runs 3.
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

POLY_LOG="logs"
TSVC_LOG="logs_tsvc"
CBENCH_LOG="logs_cbench"
mkdir -p "$POLY_LOG" "$TSVC_LOG" "$CBENCH_LOG"

ALREADY_DONE_POLYBENCH=(
  "PolyBenchC_no_rag/datamining/correlation/correlation.c"
  "PolyBenchC_no_rag/datamining/covariance/covariance.c"
  "PolyBenchC_no_rag/linear-algebra/blas/gemm/gemm.c"
  "PolyBenchC_no_rag/linear-algebra/blas/gemver/gemver.c"
  "PolyBenchC_no_rag/linear-algebra/blas/gesummv/gesummv.c"
  "PolyBenchC_no_rag/linear-algebra/blas/symm/symm.c"
  "PolyBenchC_no_rag/linear-algebra/blas/syr2k/syr2k.c"
  "PolyBenchC_no_rag/linear-algebra/blas/syrk/syrk.c"
  "PolyBenchC_no_rag/linear-algebra/blas/trmm/trmm.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/2mm/2mm.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/3mm/3mm.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/atax/atax.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/bicg/bicg.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/doitgen/doitgen.c"
  "PolyBenchC_no_rag/linear-algebra/kernels/mvt/mvt.c"
  "PolyBenchC_no_rag/linear-algebra/solvers/cholesky/cholesky.c"
  "PolyBenchC_no_rag/linear-algebra/solvers/durbin/durbin.c"
  "PolyBenchC_no_rag/linear-algebra/solvers/gramschmidt/gramschmidt.c"
)

REMAINING_POLYBENCH=(
  "PolyBenchC_no_rag/linear-algebra/solvers/ludcmp/ludcmp.c"
  "PolyBenchC_no_rag/linear-algebra/solvers/lu/lu.c"
  "PolyBenchC_no_rag/linear-algebra/solvers/trisolv/trisolv.c"
  "PolyBenchC_no_rag/medley/deriche/deriche.c"
  "PolyBenchC_no_rag/medley/floyd-warshall/floyd-warshall.c"
  "PolyBenchC_no_rag/medley/nussinov/nussinov.c"
  "PolyBenchC_no_rag/stencils/adi/adi.c"
  "PolyBenchC_no_rag/stencils/fdtd-2d/fdtd-2d.c"
  "PolyBenchC_no_rag/stencils/heat-3d/heat-3d.c"
  "PolyBenchC_no_rag/stencils/jacobi-1d/jacobi-1d.c"
  "PolyBenchC_no_rag/stencils/jacobi-2d/jacobi-2d.c"
  "PolyBenchC_no_rag/stencils/seidel-2d/seidel-2d.c"
)

run_one() {
  local prog="$1" logdir="$2"
  local name; name=$(basename "$prog" .c)
  local log="$logdir/${name}.log"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] START $name"
  python optimize.py --program "$prog" --rounds 5 --runs 3 > "$log" 2>&1
  local code=$?
  if [ $code -eq 0 ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] DONE  $name"
  else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] FAIL  $name (exit $code)"
  fi
}

print_summary() {
  local logdir="$1"; shift
  local progs=("$@")
  printf "%-24s  %-12s  %-10s\n" "benchmark" "baseline(ms)" "best"
  printf "%-24s  %-12s  %-10s\n" "---------" "------------" "----"
  for prog in "${progs[@]}"; do
    local name; name=$(basename "$prog" .c)
    local log="$logdir/${name}.log"
    [ -f "$log" ] || { printf "%-24s  %-12s  %-10s\n" "$name" "-" "not run"; continue; }
    local baseline best
    baseline=$(grep "基线 -O3:" "$log" | tail -1 | awk '{print $3}')
    best=$(grep -E "最优加速比:|组合加速比:" "$log" | tail -1 | awk '{print $2}')
    [ -z "$best" ] && best="incomplete"
    [ -z "$baseline" ] && baseline="?"
    printf "%-24s  %-12s  %-10s\n" "$name" "$baseline" "$best"
  done
}

echo "========================================"
echo "Stage 1/3: remaining PolyBench (${#REMAINING_POLYBENCH[@]} kernels), sequential, rounds=5"
echo "========================================"
for prog in "${REMAINING_POLYBENCH[@]}"; do
  run_one "$prog" "$POLY_LOG"
done

echo ""
echo "========================================"
echo "Stage 2/3: TSVC ($(wc -l < TSVC_shim/manifest.txt) kernels), sequential, rounds=5"
echo "========================================"
mapfile -t TSVC_PROGS < TSVC_shim/manifest.txt
for prog in "${TSVC_PROGS[@]}"; do
  [ -n "$prog" ] || continue
  run_one "$prog" "$TSVC_LOG"
done

echo ""
echo "========================================"
echo "Stage 3/3: cBench ($(wc -l < CBench_shim_root/manifest.txt) kernels), sequential, rounds=5"
echo "========================================"
mapfile -t CBENCH_PROGS < CBench_shim_root/manifest.txt
for prog in "${CBENCH_PROGS[@]}"; do
  [ -n "$prog" ] || continue
  run_one "$prog" "$CBENCH_LOG"
done

echo ""
echo "========================================"
echo "ALL DONE: PolyBench (30) + TSVC (${#TSVC_PROGS[@]}) + cBench (${#CBENCH_PROGS[@]})."
echo "========================================"

echo ""
echo "=== PolyBench: all 30 (18 pre-existing + 12 from this run) ==="
print_summary "$POLY_LOG" "${ALREADY_DONE_POLYBENCH[@]}" "${REMAINING_POLYBENCH[@]}"

echo ""
echo "=== TSVC: all ${#TSVC_PROGS[@]} ==="
print_summary "$TSVC_LOG" "${TSVC_PROGS[@]}"

echo ""
echo "=== cBench: all ${#CBENCH_PROGS[@]} ==="
print_summary "$CBENCH_LOG" "${CBENCH_PROGS[@]}"
