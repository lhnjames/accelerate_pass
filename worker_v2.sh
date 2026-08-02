#!/bin/bash
# Pulls tasks from the shared queue on oracle4 (10.66.66.1:8001) and runs them
# through comet's optimize.py ("kind":"comet"), the OpenCode+DeepSeek baseline
# harness ("kind":"opencode"), or the AutoPass-style pass-order-search
# baseline ("kind":"passorder"). Each WORKER_SLOT on a node gets its own
# dedicated CPU core (via --pin-cpu) so concurrent workers on the same
# 20-core machine don't contend for scheduling and add timing noise to each
# other's measurements.
set -u -o pipefail
QUEUE="http://10.66.66.1:8001"
NODE="$(hostname)-${WORKER_SLOT:-0}"
COMET_DIR="/home/hanning/comet"
LOGDIR="$COMET_DIR/logs_queue_run_v2"
OC_SCRATCH_ROOT="$COMET_DIR/opencode_runs"
PO_SCRATCH_ROOT="$COMET_DIR/passorder_runs"
mkdir -p "$LOGDIR" "$OC_SCRATCH_ROOT" "$PO_SCRATCH_ROOT"
cd "$COMET_DIR" || exit 1

# Slot -> dedicated core (spread across the 20 cores, not adjacent).
case "${WORKER_SLOT:-0}" in
  0) PIN_CPU=2 ;;
  1) PIN_CPU=9 ;;
  2) PIN_CPU=16 ;;
  *) PIN_CPU=$(( (WORKER_SLOT * 7) % 20 )) ;;
esac

jget() { python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('$1', ''))" "$2"; }

# ---------------------------------------------------------------------------
# Orphan guard.
#
# If a worker is killed (or its terminal dies) while a task is mid-run, the
# harness process it spawned is re-parented to init and keeps going forever:
# it never reports back to the queue, but it DOES keep compiling and timing
# binaries on --pin-cpu $PIN_CPU. That is the same core the next worker pins
# its own measurements to, so every subsequent timing on this node is taken
# on half a core.
#
# This is not hypothetical. On 2026-07-30 both nodes were left with one such
# orphan (dgx-spark-a PID 1539565 for 23h, dgx-spark-b PID 716932 for 21h),
# and they were still running when the entire PO sweep was measured. The
# self-calibrating control group (tasks that fell back to LLVM's own -O3
# pipeline, whose speedup must therefore be exactly 1.000) came out at
# 0.665x-1.413x on the short cBench programs -- a +/-16% noise floor.
#
# Kill only re-parented (PPID 1) harness processes: a live worker's children
# always have that worker as their parent, so this can never hit a healthy
# concurrent slot.
reap_orphans() {
  local found=0 pid ppid cmd
  while read -r pid ppid cmd; do
    [[ "$ppid" == "1" ]] || continue
    found=1
    echo "[$(date '+%H:%M:%S')] reaping orphaned harness process $pid: $cmd"
    pkill -TERM -P "$pid" 2>/dev/null
    kill -TERM "$pid" 2>/dev/null
    sleep 2
    kill -KILL "$pid" 2>/dev/null
  done < <(ps -eo pid=,ppid=,args= | grep -E 'optimize\.py|run_autopass\.py|prepare_task\.py' | grep -v grep)
  [[ "$found" == "0" ]] && echo "[$(date '+%H:%M:%S')] no orphaned harness processes"
}

# Anything this worker spawned dies with it, so a future worker never has to
# reap us. Covers SIGINT/SIGTERM and normal loop exit alike.
cleanup_own_children() {
  pkill -TERM -P $$ 2>/dev/null
}
trap 'cleanup_own_children; exit 130' INT TERM
trap 'cleanup_own_children' EXIT

reap_orphans

while true; do
  task_json=$(curl -sf "$QUEUE/next?node=$NODE")
  if [[ -z "$task_json" ]]; then
    echo "[$(date '+%H:%M:%S')] queue unreachable, retrying in 30s"
    sleep 30
    continue
  fi
  done_flag=$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('done', False))" "$task_json")
  if [[ "$done_flag" == "True" ]]; then
    echo "[$(date '+%H:%M:%S')] no more pending tasks, worker $NODE exiting"
    break
  fi

  id=$(jget id "$task_json")
  kind=$(jget kind "$task_json")
  dataset=$(jget dataset "$task_json")
  program=$(jget program "$task_json")
  rounds=$(jget rounds "$task_json")
  runs=$(jget runs "$task_json")
  extra_args_json=$(python3 -c "import json,sys; print(json.dumps(json.loads(sys.argv[1]).get('extra_args', [])))" "$task_json")

  log="$LOGDIR/${id}.log"
  echo "[$(date '+%H:%M:%S')] START $id kind=$kind ($program) on $NODE pin_cpu=$PIN_CPU"

  if [[ "$kind" == "opencode" ]]; then
    scratch="$OC_SCRATCH_ROOT/$id"
    rm -rf "$scratch"
    "$COMET_DIR/scripts/opencode_harness/run_one.sh" "$program" "$scratch" "$rounds" "$runs" 600 "$PIN_CPU" \
      > "$log" 2>&1
    code=$?
  elif [[ "$kind" == "passorder" ]]; then
    scratch="$PO_SCRATCH_ROOT/$id"
    rm -rf "$scratch"
    "$COMET_DIR/.venv/bin/python3" "$COMET_DIR/scripts/passorder_search/prepare_task.py" \
      "$program" "$scratch" "$PIN_CPU" > "$log" 2>&1
    prep_code=$?
    if [[ "$prep_code" == "0" ]]; then
      # rounds hardcoded to 3 (R3) regardless of the task's own "rounds"
      # field (9, set back when this used the old run_one.py strawman) --
      # AutoPass's paper reports its headline 1.04x-1.15x geomean numbers
      # at a 3-round budget specifically; using 9 here would no longer be
      # reproducing what those numbers describe.
      "$COMET_DIR/.venv/bin/python3" "$COMET_DIR/scripts/passorder_search/run_autopass.py" \
        "$program" "$scratch" 3 "$runs" "$PIN_CPU" >> "$log" 2>&1
      code=$?
    else
      code=$prep_code
    fi
  else
    mapfile -t extra_args < <(python3 -c "import json,sys; [print(a) for a in json.loads(sys.argv[1])]" "$extra_args_json")
    "$COMET_DIR/.venv/bin/python3" optimize.py --program "$program" --rounds "$rounds" --runs "$runs" \
      --dataset "$dataset" --pin-cpu "$PIN_CPU" "${extra_args[@]}" > "$log" 2>&1
    code=$?
  fi

  echo "[$(date '+%H:%M:%S')] DONE $id exit=$code"
  status="done"
  [[ "$code" != "0" ]] && status="failed"
  curl -sf -X POST "$QUEUE/done" -H "Content-Type: application/json" \
    -d "{\"id\":\"$id\",\"node\":\"$NODE\",\"status\":\"$status\",\"exit_code\":$code}" >/dev/null
done
