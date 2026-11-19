#!/bin/bash
# Fig.15 — YCSB A–F + 100% insert + 100% range (paper §4.3).
# DPA-Store only by default. ROLEX baseline is opt-in (FIG15_ROLEX=1).
#
# Restart server before every workload: sustained UPDATE/INSERT trips DPA
# RPC Fatal 0x4; reusing the dead process zeros all later rows.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"
require_file "$TRACE_WORK_DIR/update.trace"
require_file "$TRACE_WORK_DIR/insert.trace"
require_file "$TRACE_WORK_DIR/range.trace"

CSV="$OUT_DIR/fig15_ycsb.csv"
echo "dataset,system,workload,run,mops,p50_us,p90_us,p99_us" >"$CSV"

run_one_wl() {
  local ds=$1 wl=$2 mop=$3 lop=$4 one=$5 inf=$6 mt=$7 lt=$8
  local ops=${9:-$OPS_PER_THREAD_GET}
  local tag="fig15_${ds}_dpastore_${wl}"
  for run in $(seq 1 "$NUM_RUNS_LONG"); do
    # Fresh server every run (WRITE / mixed WLs trip DPA Fatal after one go).
    start_server "$SERVER_LOG_DIR/${tag}_r${run}_server.log"
    RUN_TAG="${tag}_r${run}" \
    MAIN_TRACE="$mt" LOOP_TRACE="$lt" \
    MAIN_OP=$mop LOOP_OP=$lop MAIN_ONE_OF=$one \
    NUM_INFLIGHT=$inf OPS_PER_THREAD=$ops \
      run_client || true
    if metrics=$(parse_client_log "$CLIENT_LOG_DIR/${tag}_r${run}.log"); then
      read -r mops p50 p90 p99 <<<"$metrics"
      echo "$ds,dpastore,$wl,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
    else
      log "WARN: parse failed $tag run=$run"
      echo "$ds,dpastore,$wl,$run,nan,nan,nan,nan" | tee -a "$CSV"
    fi
    stop_server || true
  done
}

run_ycsb_dpastore() {
  local ds=$1
  local get_t=$2 upd_t=$3 ins_t=$4 range_t=$5
  local bulk=$6
  local eps=${7:-default}

  stop_server || true
  sudo cp -f "$bulk" "$BULK_INSTALL_PATH"

  # Non-range workloads (RANGE_REQUEST=0 keeps pkt small)
  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 \
    RANGE=0 EPSILON_MODE=$eps BTREE=0

  # ops budget: GET uses OPS_PER_THREAD_GET; pure/mixed writes use WRITE budget
  # so runs finish before DPA write-path collapse (matches fig13 insert ≈4.8 Mops).
  declare -a JOBS=(
    "A:update:get:2:$NUM_INFLIGHT_WRITE:$upd_t:$get_t:$OPS_PER_THREAD_WRITE"
    "B:update:get:20:$NUM_INFLIGHT_WRITE:$upd_t:$get_t:$OPS_PER_THREAD_WRITE"
    "C:get:get:1:$NUM_INFLIGHT_GET:$get_t:$get_t:$OPS_PER_THREAD_GET"
    "D:insert:get:20:$NUM_INFLIGHT_WRITE:$ins_t:$get_t:$OPS_PER_THREAD_WRITE"
    "F:update:get:2:$NUM_INFLIGHT_WRITE:$upd_t:$get_t:$OPS_PER_THREAD_WRITE"
    "insert100:insert:insert:1:$NUM_INFLIGHT_WRITE:$ins_t:$ins_t:$OPS_PER_THREAD_WRITE"
  )

  for job in "${JOBS[@]}"; do
    IFS=':' read -r wl mop lop one inf mt lt ops <<<"$job"
    run_one_wl "$ds" "$wl" "$mop" "$lop" "$one" "$inf" "$mt" "$lt" "$ops"
  done

  if [ "${FIG15_SKIP_RANGE:-0}" = "1" ]; then
    log "skipping YCSB-E / range100 (FIG15_SKIP_RANGE=1)"
    return 0
  fi

  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 \
    RANGE=1 EPSILON_MODE=$eps BTREE=0
  sudo cp -f "$bulk" "$BULK_INSTALL_PATH"

  for job in \
      "E:insert:range:20:$NUM_INFLIGHT_WRITE:$ins_t:$range_t:$OPS_PER_THREAD_WRITE" \
      "range100:range:range:1:$NUM_INFLIGHT_WRITE:$range_t:$range_t:$OPS_PER_THREAD_WRITE"; do
    IFS=':' read -r wl mop lop one inf mt lt ops <<<"$job"
    run_one_wl "$ds" "$wl" "$mop" "$lop" "$one" "$inf" "$mt" "$lt" "$ops"
  done
}

run_ycsb_dpastore sparse \
  "$TRACE_WORK_DIR/get.trace" \
  "$TRACE_WORK_DIR/update.trace" \
  "$TRACE_WORK_DIR/insert.trace" \
  "$TRACE_WORK_DIR/range.trace" \
  "$BULK_TRACE" default

for ds in amzn osmc; do
  g=$TRACE_WORK_DIR/datasets/$ds/get.trace
  u=$TRACE_WORK_DIR/datasets/$ds/update.trace
  i=$TRACE_WORK_DIR/datasets/$ds/insert.trace
  r=$TRACE_WORK_DIR/datasets/$ds/range.trace
  b=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  [ -f "$g" ] && [ -f "$b" ] || continue
  [ -f "$u" ] || u=$g
  [ -f "$i" ] || i=$TRACE_WORK_DIR/insert.trace
  [ -f "$r" ] || r=$TRACE_WORK_DIR/range.trace
  eps=default; [ "$ds" = "osmc" ] && eps=16
  run_ycsb_dpastore "$ds" "$g" "$u" "$i" "$r" "$b" "$eps"
done

if [ "${FIG15_ROLEX:-0}" = "1" ]; then
  log "FIG15_ROLEX=1 requested — ensure ROLEX server is running, then extend this script."
  log "Skipped automatic ROLEX sweep (needs RDMA + third_party/rolex server)."
fi

restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig 15 --data-dir "$OUT_DIR"
log "Fig.15 done → $CSV"
