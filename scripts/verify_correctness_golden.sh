#!/bin/bash
# Independent golden-standard correctness check for an LLM-rewritten PolyBench
# kernel -- deliberately NOT using COMET's own correctness gate.
#
# Both the original and the rewritten source are compiled at -O3 with
# -DPOLYBENCH_DUMP_ARRAYS, run, and their full dumped arrays compared value by
# value.  A speedup is only trustworthy if this reports PASS: a wrong-but-fast
# rewrite is exactly the failure mode the experiment must never bank as a gain.
#
# Usage:
#   verify_correctness_golden.sh <program_stem> <optimized_c_abspath>
# Example:
#   verify_correctness_golden.sh gramschmidt \
#     /home/hanning/comet/runs/<ts>_polybench_gramschmidt/outputs/gramschmidt_optimized.c
#
# Run this AFTER the ablation sweep finishes -- running it mid-sweep would
# contend for CPU/memory bandwidth with the cell currently being timed.
set -euo pipefail

STEM="${1:?program stem, e.g. gramschmidt}"
OPT="${2:?absolute path to *_optimized.c}"
ROOT=/home/hanning/comet
U="$ROOT/PolyBenchC_no_rag/utilities"

# Locate the original kernel .c for this stem inside the PolyBench tree.
ORIG=$(find "$ROOT/PolyBenchC_no_rag" -name "${STEM}.c" -not -path '*/utilities/*' | head -1)
S=$(dirname "$ORIG")
[ -f "$ORIG" ] || { echo "FAIL: original ${STEM}.c not found"; exit 2; }
[ -f "$OPT" ]  || { echo "FAIL: optimized source not found: $OPT"; exit 2; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "program:   $STEM"
echo "original:  $ORIG"
echo "optimized: $OPT"

/usr/bin/clang-21 -O3 -I "$U" -I "$S" -DLARGE_DATASET -DPOLYBENCH_DUMP_ARRAYS \
  "$ORIG" "$U/polybench.c" -o "$T/ref" -lm
/usr/bin/clang-21 -O3 -I "$U" -I "$S" -DLARGE_DATASET -DPOLYBENCH_DUMP_ARRAYS \
  "$OPT" "$U/polybench.c" -o "$T/cand" -lm

"$T/ref"  2> "$T/ref.out"
"$T/cand" 2> "$T/cand.out"
echo "ref bytes: $(wc -c < "$T/ref.out")  cand bytes: $(wc -c < "$T/cand.out")"

if cmp -s "$T/ref.out" "$T/cand.out"; then
  echo "RESULT: BIT-IDENTICAL"
fi

python3 - "$T/ref.out" "$T/cand.out" <<'PY'
import sys, re
def nums(p):
    txt = open(p, errors="replace").read()
    return [float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", txt)]
a, b = nums(sys.argv[1]), nums(sys.argv[2])
print("value counts:", len(a), len(b))
if len(a) != len(b):
    print("VERDICT: FAIL (count mismatch)"); sys.exit(1)
if not a:
    print("VERDICT: FAIL (no numeric output dumped)"); sys.exit(1)
m = max(abs(x-y)/max(abs(x), 1e-30) for x, y in zip(a, b))
print("max relative error: %.3e" % m)
print("VERDICT:", "PASS (<=1e-12)" if m <= 1e-12
      else "PASS (<=1e-6)" if m <= 1e-6 else "FAIL (>1e-6)")
PY
