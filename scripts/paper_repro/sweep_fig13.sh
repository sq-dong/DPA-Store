#!/bin/bash
# Fig.13 — UPDATE/INSERT mixes with GET (paper §4.2.8).
# Mixes: 100% / 50% / 5% update|insert (remainder GET).
# main_one_of: 100%→1, 50%→2, 5%→20
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"
require_file "$TRACE_WORK_DIR/update.trace"
require_file "$TRACE_WORK_DIR/insert.trace"

CSV="$OUT_DIR/fig13_insert_update.csv"
echo "dataset,mix,run,mops,p50_us,p90_us,p99_us" >"$CSV"

# Write mixes stress DPA; restart server for every mix (Fatal 0x4 after sustained UPDATE).
rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 EPSILON_MODE=default BTREE=0

run_mix() {
  local ds=$1 mix=$2 main_trace=$3 main_op=$4 one_of=$5
  local tag="fig13_${ds}_${mix}"
  local runs=$NUM_RUNS_LONG
  case "$mix" in
    *100pct_insert*|*100pct_update*) runs=$NUM_RUNS_SHORT ;;
  esac
  for run in $(seq 1 "$runs"); do
    # Fresh server every run — WRITE Fatal 0x4 after a single sustained mix.
    start_server "$SERVER_LOG_DIR/${tag}_r${run}_server.log"
    RUN_TAG="${tag}_r${run}" \
    MAIN_TRACE="$main_trace" LOOP_TRACE="$TRACE_WORK_DIR/get.trace" \
    MAIN_OP=$main_op LOOP_OP=get MAIN_ONE_OF=$one_of \
    NUM_INFLIGHT=$NUM_INFLIGHT_WRITE OPS_PER_THREAD=$OPS_PER_THREAD_WRITE \
      run_client || true
    if metrics=$(parse_client_log "$CLIENT_LOG_DIR/${tag}_r${run}.log"); then
      read -r mops p50 p90 p99 <<<"$metrics"
      echo "$ds,$mix,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
    else
      log "WARN: parse failed $tag run=$run"
      echo "$ds,$mix,$run,nan,nan,nan,nan" | tee -a "$CSV"
    fi
    stop_server || true
  done
}

# sparse only by default
DS=sparse
run_mix "$DS" update_100pct "$TRACE_WORK_DIR/update.trace" update 1
run_mix "$DS" update_50pct  "$TRACE_WORK_DIR/update.trace" update 2
run_mix "$DS" update_5pct   "$TRACE_WORK_DIR/update.trace" update 20
run_mix "$DS" insert_100pct "$TRACE_WORK_DIR/insert.trace" insert 1
run_mix "$DS" insert_50pct  "$TRACE_WORK_DIR/insert.trace" insert 2
run_mix "$DS" insert_5pct   "$TRACE_WORK_DIR/insert.trace" insert 20

# Extra datasets: restart server with their bulk if available
for ds in amzn osmc; do
  g=$TRACE_WORK_DIR/datasets/$ds/get.trace
  u=$TRACE_WORK_DIR/datasets/$ds/update.trace
  i=$TRACE_WORK_DIR/datasets/$ds/insert.trace
  b=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  [ -f "$g" ] && [ -f "$u" ] && [ -f "$i" ] && [ -f "$b" ] || continue
  stop_server
  eps=default; [ "$ds" = "osmc" ] && eps=16
  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 EPSILON_MODE=$eps BTREE=0
  sudo cp -f "$b" "$BULK_INSTALL_PATH"
  start_server "$SERVER_LOG_DIR/fig13_${ds}_server.log"
  # temporarily point get.trace path used inside run_mix via override of TRACE file — patch local refs
  TRACE_WORK_DIR_SAVE=$TRACE_WORK_DIR
  # inline specialized calls
  for mix_spec in \
      "update_100pct:$u:update:1" \
      "update_50pct:$u:update:2" \
      "update_5pct:$u:update:20" \
      "insert_100pct:$i:insert:1" \
      "insert_50pct:$i:insert:2" \
      "insert_5pct:$i:insert:20"; do
    mix=${mix_spec%%:*}; rest=${mix_spec#*:}
    mt=${rest%%:*}; rest=${rest#*:}
    mop=${rest%%:*}; one=${rest##*:}
    for run in $(seq 1 "$NUM_RUNS_LONG"); do
      RUN_TAG="fig13_${ds}_${mix}_r${run}" \
      MAIN_TRACE="$mt" LOOP_TRACE="$g" \
      MAIN_OP=$mop LOOP_OP=get MAIN_ONE_OF=$one \
      NUM_INFLIGHT=$NUM_INFLIGHT_WRITE OPS_PER_THREAD=$OPS_PER_THREAD_WRITE \
        run_client || true
      read -r mops p50 p90 p99 <<<"$(parse_client_log "$CLIENT_LOG_DIR/fig13_${ds}_${mix}_r${run}.log")"
      echo "$ds,$mix,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
    done
  done
  TRACE_WORK_DIR=$TRACE_WORK_DIR_SAVE
done

stop_server
restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig 13 --data-dir "$OUT_DIR"
log "Fig.13 done → $CSV"
