#!/bin/bash
# Fig.10 — client queue depth (paper §4.2.3). No server rebuild.
# Paper X: per-thread QD with 186 threads (6×31).
# Here: NUM_SHARD_THREADS (93) with scaled QD so total in-flight matches:
#   our_qd = ceil(PAPER_TOTAL_THREADS * paper_qd / NUM_SHARD_THREADS)
# CSV keeps both paper_queue_depth (plot X) and queue_depth (actual flag).
#
# Restart server before every client run: sustained GET load can trip DPA
# RPC Fatal 0x4; reusing a dead process makes all later points look like 0 Mops.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"

CSV="$OUT_DIR/fig10_queue_depth.csv"
echo "paper_queue_depth,queue_depth,total_inflight,run,mops,p50_us,p90_us,p99_us" >"$CSV"

PAPER_QDS=(1 2 4 8 16 24 28 32 64)
[ "$QUICK" = "1" ] && PAPER_QDS=(1 8 32)

for paper_qd in "${PAPER_QDS[@]}"; do
  # ceil(PAPER_TOTAL_THREADS * paper_qd / NUM_SHARD_THREADS)
  qd=$(( (PAPER_TOTAL_THREADS * paper_qd + NUM_SHARD_THREADS - 1) / NUM_SHARD_THREADS ))
  total_inflight=$((NUM_SHARD_THREADS * qd))
  for run in $(seq 1 "$NUM_RUNS_LONG"); do
    start_server "$SERVER_LOG_DIR/fig10_pqd${paper_qd}_r${run}_server.log"
    RUN_TAG="fig10_pqd${paper_qd}_qd${qd}_r${run}" \
    MAIN_TRACE="$TRACE_WORK_DIR/get.trace" LOOP_TRACE="$TRACE_WORK_DIR/get.trace" \
    MAIN_OP=get LOOP_OP=get MAIN_ONE_OF=1 \
    NUM_INFLIGHT=$qd OPS_PER_THREAD=$OPS_PER_THREAD_GET \
      run_client || true
    if metrics=$(parse_client_log "$CLIENT_LOG_DIR/fig10_pqd${paper_qd}_qd${qd}_r${run}.log"); then
      read -r mops p50 p90 p99 <<<"$metrics"
      echo "$paper_qd,$qd,$total_inflight,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
    else
      log "WARN: parse failed for fig10 pqd=$paper_qd run=$run (see client log)"
      echo "$paper_qd,$qd,$total_inflight,$run,nan,nan,nan,nan" | tee -a "$CSV"
    fi
  done
done

stop_server || true
log "Fig.10 CSV → $CSV (threads=$NUM_SHARD_THREADS; paper total threads=$PAPER_TOTAL_THREADS)"
python3 "$DIR/plot_paper.py" --fig 10 --data-dir "$OUT_DIR"
