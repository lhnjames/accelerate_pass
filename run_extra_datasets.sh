#!/bin/bash
# Run TSVC + cBench through COMET, sequentially, only after the PolyBench
# batch (run_polybench.sh) has fully finished. No concurrency: one kernel
# at a time, TSVC first then cBench, so this never competes with the
# PolyBench run for CPU/timing-measurement noise.
#
# Kernels come from scripts/gen_tsvc_kernels.py / scripts/gen_cbench_kernels.py
# manifests (TSVC_shim/manifest.txt, CBench_shim_root/manifest.txt) -- each
# entry is a synthetic PolyBench-shaped kernel file that the existing
# optimize.py/tune_param.py/tune_source.py pipeline runs completely
# unmodified (see scripts/gen_*.py docstrings for how).
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TSVC_LOG="logs_tsvc"
CBENCH_LOG="logs_cbench"
mkdir -p "$TSVC_LOG" "$CBENCH_LOG"

wait_for_polybench() {
  echo "[$(date '+%H:%M:%S')] waiting for run_polybench.sh to finish..."
  while pgrep -f "optimize.py --program PolyBenchC" >/dev/null 2>&1; do
    sleep 60
  done
  echo "[$(date '+%H:%M:%S')] PolyBench batch finished, starting TSVC + cBench."
}

run_one() {
  local prog="$1" logdir="$2"
  local name
  name=$(basename "$prog" .c)
  local log="$logdir/${name}.log"
  echo "[$(date '+%H:%M:%S')] START $name"
  python optimize.py --program "$prog" --rounds 5 --runs 3 > "$log" 2>&1
  local code=$?
  if [ $code -eq 0 ]; then
    echo "[$(date '+%H:%M:%S')] DONE  $name"
  else
    echo "[$(date '+%H:%M:%S')] FAIL  $name (exit $code)"
  fi
}

wait_for_polybench

echo ""
echo "========================================"
echo "TSVC: $(wc -l < TSVC_shim/manifest.txt) kernels, sequential"
echo "========================================"
while IFS= read -r prog; do
  [ -n "$prog" ] || continue
  run_one "$prog" "$TSVC_LOG"
done < TSVC_shim/manifest.txt

echo ""
echo "========================================"
echo "cBench: $(wc -l < CBench_shim_root/manifest.txt) kernels, sequential"
echo "========================================"
while IFS= read -r prog; do
  [ -n "$prog" ] || continue
  run_one "$prog" "$CBENCH_LOG"
done < CBench_shim_root/manifest.txt

echo ""
echo "========================================"
echo "TSVC + cBench batch complete."
echo "========================================"
