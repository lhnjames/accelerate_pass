#!/bin/bash
# Full validation run: SPEC + PolyBench + CBench, sequential-per-slot with
# bounded concurrency (same worker pattern as run_polybench.sh / run_spec.sh),
# meant to be launched once inside a tmux session and left running.
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOGDIR="logs_full_validation"
mkdir -p "$LOGDIR"

mapfile -t SPEC_PROGS    < SPEC_shim_root/manifest.txt
mapfile -t CBENCH_PROGS  < CBench_shim_root/manifest.txt

POLYBENCH_PROGS=(
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

ALL_PROGS=("${SPEC_PROGS[@]}" "${POLYBENCH_PROGS[@]}" "${CBENCH_PROGS[@]}")

# Only 4 cores on this box (see `nproc`); SPEC kernels (mcf_r especially,
# baseline ~9.4s, now up to 10-20 -mllvm candidates per try_flags step)
# run far longer per candidate than most PolyBench/CBench kernels, so
# concurrent timing runs contend for CPU more than the original
# PolyBench-only script (CONCURRENCY=3) accepted -- default lower here to
# keep wall-clock timing measurements less noisy. Override with
# `CONCURRENCY=N bash run_full_validation.sh` if you want to trade
# accuracy for wall-clock time.
CONCURRENCY=${CONCURRENCY:-2}

run_one() {
  local prog="$1"
  local name; name=$(basename "$prog" .c)
  local log="$LOGDIR/${name}.log"
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] START $name"
  python3 optimize.py --program "$prog" --rounds 5 --runs 3 > "$log" 2>&1
  local code=$?
  if [ $code -eq 0 ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] DONE  $name"
  else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] FAIL  $name (exit $code)"
  fi
}

echo "========================================"
echo "Full validation: ${#ALL_PROGS[@]} kernels"
echo "  SPEC=${#SPEC_PROGS[@]}  PolyBench=${#POLYBENCH_PROGS[@]}  CBench=${#CBENCH_PROGS[@]}"
echo "  concurrency=$CONCURRENCY"
echo "========================================"

for prog in "${ALL_PROGS[@]}"; do
  [ -n "$prog" ] || continue
  # `wait -n` frees a slot as soon as ANY running job finishes, not just
  # the first one queued -- kernels here range from ~1min (small PolyBench)
  # to 1hr+ (mcf_r), so waiting on a fixed pid (as the original
  # run_polybench.sh pattern does) can leave a free CPU idle for a long
  # time behind a slow kernel that just happens to be first in the queue.
  while [ "$(jobs -rp | wc -l)" -ge "$CONCURRENCY" ]; do
    wait -n
  done
  run_one "$prog" &
done
wait

echo ""
echo "========================================"
echo "ALL DONE: ${#ALL_PROGS[@]} kernels."
echo "========================================"
echo ""
echo "=== RESULTS SUMMARY ==="
printf "%-28s  %-12s  %-10s\n" "benchmark" "baseline(ms)" "best"
printf "%-28s  %-12s  %-10s\n" "---------" "------------" "----"
for prog in "${ALL_PROGS[@]}"; do
  [ -n "$prog" ] || continue
  name=$(basename "$prog" .c)
  log="$LOGDIR/${name}.log"
  [ -f "$log" ] || { printf "%-28s  %-12s  %-10s\n" "$name" "-" "not run"; continue; }
  baseline=$(grep "基线 -O3:" "$log" | tail -1 | awk '{print $3}')
  best=$(grep -E "最优加速比:|组合加速比:" "$log" | tail -1 | awk '{print $2}')
  [ -z "$best" ] && best="incomplete"
  [ -z "$baseline" ] && baseline="?"
  printf "%-28s  %-12s  %-10s\n" "$name" "$baseline" "$best"
done
