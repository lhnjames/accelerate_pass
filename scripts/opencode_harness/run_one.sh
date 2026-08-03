#!/bin/bash
# Run the OpenCode+DeepSeek baseline for one comet benchmark program, driven
# by an explicit N-round loop (matching comet's --rounds budget) rather than
# a single long autonomous session capped by wall-clock time.
# Usage: run_one.sh <program_rel_path> <scratch_dir> [rounds] [confirm_runs] [round_timeout_s] [pin_cpu]
set -u -o pipefail
PROGRAM="$1"
SCRATCH="$2"
ROUNDS="${3:-9}"           # matches --rounds 9 used for conditions 1/2/3
CONFIRM_RUNS="${4:-3}"     # matches --runs 3 used for conditions 1/2/3
ROUND_TIMEOUT_S="${5:-600}"  # safety cap per round, not the driving budget
PIN_CPU="${6:-}"           # dedicated core for this worker's compile/time subprocesses

export PATH="$HOME/Software/nodejs/bin:$PATH"
export DEEPSEEK_API_KEY="$(grep '^DEEPSEEK_API_KEY=' /home/hanning/comet/.env | cut -d= -f2)"

# Fail loudly if the agent under test is not installed.
#
# It was not installed on dgx-spark-b, and because each round was launched as
# `timeout ... opencode run ...` with stdout redirected to a file, the missing
# binary produced an empty round_N.jsonl and a one-line stderr, the loop ran
# all 9 rounds in about two seconds, and finalize.py then dutifully measured
# the UNTOUCHED kernel.c against itself and reported ~1.00x as a result. 43 of
# the 49 OpenCode tasks were scored that way -- an entire baseline condition,
# and the "generic agents can't do compiler optimization" claim resting on it,
# came from a missing executable. Exit non-zero so the queue records a failure
# instead of a number.
if ! command -v opencode >/dev/null 2>&1; then
  echo "[FATAL] 'opencode' not found on $(hostname) (PATH=$PATH)." >&2
  echo "        Refusing to 'measure' an unedited kernel as if an agent had run." >&2
  exit 127
fi
if [[ -z "${DEEPSEEK_API_KEY:-}" ]]; then
  echo "[FATAL] DEEPSEEK_API_KEY is empty; opencode would run without a model." >&2
  exit 127
fi

VENV_PY=/home/hanning/comet/.venv/bin/python3
MODEL="deepseek/deepseek-v4-pro"

echo "[$(date '+%H:%M:%S')] preparing $PROGRAM -> $SCRATCH (pin_cpu=${PIN_CPU:-none})"
"$VENV_PY" /home/hanning/comet/scripts/opencode_harness/prepare_task.py "$PROGRAM" "$SCRATCH" "$PIN_CPU" || exit 1

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
    # Scan the whole file for a session id, not just line 1: the id does not
    # reliably appear on the first record, and when this came back empty the
    # loop silently degraded to a FRESH session every round -- so the agent had
    # no memory of its own prior attempts and the round prompt's "revert to
    # your best-so-far version" was an instruction it could not act on.
    SESSION_ID=$("$VENV_PY" - "round_${round}.jsonl" <<'PY'
import json, sys
sid = ""
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except Exception:
        continue
    for key in ("sessionID", "sessionId", "session_id"):
        if isinstance(obj, dict) and obj.get(key):
            sid = obj[key]
            break
    if sid:
        break
print(sid)
PY
)
    if [[ -z "$SESSION_ID" ]]; then
      echo "[warn] no session id captured in round 1; later rounds will have no memory of earlier ones" >&2
    fi
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
