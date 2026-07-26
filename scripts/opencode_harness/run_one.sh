#!/bin/bash
# Run the OpenCode+DeepSeek baseline for one comet benchmark program, driven
# by an explicit N-round loop (matching comet's --rounds budget) rather than
# a single long autonomous session capped by wall-clock time.
# Usage: run_one.sh <program_rel_path> <scratch_dir> [rounds] [confirm_runs] [round_timeout_s]
set -u -o pipefail
PROGRAM="$1"
SCRATCH="$2"
ROUNDS="${3:-9}"           # matches --rounds 9 used for conditions 1/2/3
CONFIRM_RUNS="${4:-3}"     # matches --runs 3 used for conditions 1/2/3
ROUND_TIMEOUT_S="${5:-600}"  # safety cap per round, not the driving budget

export PATH="$HOME/Software/nodejs/bin:$PATH"
export DEEPSEEK_API_KEY="$(grep '^DEEPSEEK_API_KEY=' /home/hanning/comet/.env | cut -d= -f2)"

VENV_PY=/home/hanning/comet/.venv/bin/python3
MODEL="deepseek/deepseek-v4-pro"

echo "[$(date '+%H:%M:%S')] preparing $PROGRAM -> $SCRATCH"
"$VENV_PY" /home/hanning/comet/scripts/opencode_harness/prepare_task.py "$PROGRAM" "$SCRATCH" || exit 1

cd "$SCRATCH" || exit 1
SESSION_ID=""

for round in $(seq 1 "$ROUNDS"); do
  if [[ "$round" == "1" ]]; then
    msg="$(cat prompt.txt)

This is round 1/$ROUNDS. Make your first optimization attempt to kernel.c, then run ./measure.sh to see the result."
    echo "[$(date '+%H:%M:%S')] round $round/$ROUNDS (new session)..."
    timeout "$ROUND_TIMEOUT_S" opencode run "$msg" \
      --model "$MODEL" --dir "$SCRATCH" -f kernel.c --auto --format json \
      > "round_${round}.jsonl" 2>"round_${round}.stderr"
    SESSION_ID=$(head -1 "round_${round}.jsonl" 2>/dev/null | python3 -c "import json,sys; print(json.loads(sys.stdin.read() or '{}').get('sessionID',''))" 2>/dev/null)
  else
    msg="Round $round/$ROUNDS. Here is your current ./measure.sh result: $(./measure.sh 2>&1 | tail -1)

If you believe this is a genuine improvement over your previous attempts, keep it. If not, revert to your best-so-far version of kernel.c. Then either make ONE more optimization attempt, or if you're confident further changes won't help, say so explicitly and make no further edits."
    if [[ -n "$SESSION_ID" ]]; then
      echo "[$(date '+%H:%M:%S')] round $round/$ROUNDS (session $SESSION_ID)..."
      timeout "$ROUND_TIMEOUT_S" opencode run "$msg" \
        --model "$MODEL" --dir "$SCRATCH" --session "$SESSION_ID" --auto --format json \
        > "round_${round}.jsonl" 2>"round_${round}.stderr"
    else
      echo "[$(date '+%H:%M:%S')] round $round/$ROUNDS (no session id captured, fresh call)..."
      timeout "$ROUND_TIMEOUT_S" opencode run "$msg" \
        --model "$MODEL" --dir "$SCRATCH" -f kernel.c --auto --format json \
        > "round_${round}.jsonl" 2>"round_${round}.stderr"
    fi
  fi
done

echo "[$(date '+%H:%M:%S')] all $ROUNDS rounds done, finalizing (confirm runs=$CONFIRM_RUNS)..."
"$VENV_PY" /home/hanning/comet/scripts/opencode_harness/finalize.py "$SCRATCH" "$CONFIRM_RUNS" > "$SCRATCH/result.json"
cat "$SCRATCH/result.json"
