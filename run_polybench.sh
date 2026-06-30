#!/bin/bash
# Run full PolyBench suite: 3 parallel workers
set -e
cd /home/hanning/accelerate/comet

LOGDIR="logs"
mkdir -p "$LOGDIR"

PROGS=(
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

CONCURRENCY=3
pids=()

run_one() {
  local prog="$1"
  local name
  name=$(basename "$prog" .c)
  local log="$LOGDIR/${name}.log"
  echo "[$(date '+%H:%M:%S')] START $name"
  python optimize.py --program "$prog" --rounds 9 --runs 3 > "$log" 2>&1
  local code=$?
  if [ $code -eq 0 ]; then
    echo "[$(date '+%H:%M:%S')] DONE  $name"
  else
    echo "[$(date '+%H:%M:%S')] FAIL  $name (exit $code)"
  fi
}

for prog in "${PROGS[@]}"; do
  run_one "$prog" &
  pids+=($!)
  if [ ${#pids[@]} -ge "$CONCURRENCY" ]; then
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
done

for pid in "${pids[@]}"; do
  wait "$pid"
done

echo ""
echo "========================================"
echo "All 30 benchmarks complete."
echo "========================================"
echo ""
echo "=== RESULTS SUMMARY ==="
printf "%-22s  %-12s  %-8s  %-8s\n" "benchmark" "baseline(ms)" "param" "source"
printf "%-22s  %-12s  %-8s  %-8s\n" "---------" "------------" "-----" "------"
for prog in "${PROGS[@]}"; do
  name=$(basename "$prog" .c)
  log="$LOGDIR/${name}.log"
  [ -f "$log" ] || continue
  baseline=$(grep "Baseline -O3:" "$log" | tail -1 | awk '{print $3}')
  param=$(grep "Param flags:" "$log" | tail -1 | grep -oP '\(\K[0-9.]+(?=x\))')
  source=$(grep "Source best:" "$log" | tail -1 | grep -oP '^[0-9.]+(?=x)')
  [ -z "$source" ] && source="none"
  [ -z "$param"  ] && param="1.000"
  printf "%-22s  %-12s  %-8s  %-8s\n" "$name" "$baseline" "${param}x" "${source}x"
done
