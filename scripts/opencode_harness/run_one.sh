#!/bin/bash
# Run the OpenCode+DeepSeek baseline for one comet benchmark program.
# Usage: run_one.sh <program_rel_path> <scratch_dir> [timeout_seconds] [confirm_runs]
set -u -o pipefail
PROGRAM="$1"
SCRATCH="$2"
TIMEOUT_S="${3:-2700}"     # 45 min default, matches comet's median task time
CONFIRM_RUNS="${4:-3}"     # matches --runs 3 used for conditions 1/2/3

export PATH="$HOME/Software/nodejs/bin:$PATH"
export DEEPSEEK_API_KEY="$(grep '^DEEPSEEK_API_KEY=' /home/hanning/comet/.env | cut -d= -f2)"

VENV_PY=/home/hanning/comet/.venv/bin/python3

echo "[$(date '+%H:%M:%S')] preparing $PROGRAM -> $SCRATCH"
"$VENV_PY" /home/hanning/comet/scripts/opencode_harness/prepare_task.py "$PROGRAM" "$SCRATCH" || exit 1

cd "$SCRATCH" || exit 1
echo "[$(date '+%H:%M:%S')] running opencode (timeout ${TIMEOUT_S}s)..."
timeout "$TIMEOUT_S" opencode run "$(cat prompt.txt)" \
    --model deepseek/deepseek-v4-pro --dir "$SCRATCH" -f kernel.c --auto \
    > "$SCRATCH/opencode_session.log" 2>&1
code=$?
echo "[$(date '+%H:%M:%S')] opencode exited with $code"

echo "[$(date '+%H:%M:%S')] finalizing (confirm runs=$CONFIRM_RUNS)..."
"$VENV_PY" /home/hanning/comet/scripts/opencode_harness/finalize.py "$SCRATCH" "$CONFIRM_RUNS" > "$SCRATCH/result.json"
cat "$SCRATCH/result.json"
