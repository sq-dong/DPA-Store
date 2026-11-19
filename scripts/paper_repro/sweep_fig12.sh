#!/bin/bash
# Fig.12 — Learned vs B+-tree traversal (paper §4.2.5).
# B+ path: EPSILON_MODE=inf (packed nodes) + USE_BTREE_ALGORITHM.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"

CSV="$OUT_DIR/fig12_learned_vs_btree.csv"
echo "dataset,variant,run,mops,p50_us,p90_us,p99_us" >"$CSV"

run_variant() {
  local ds=$1 variant=$2 btree=$3 eps=$4 get_trace=$5 bulk=$6
  local tag="fig12_${ds}_${variant}"
  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 \
    EPSILON_MODE=$eps BTREE=$btree
  sudo cp -f "$bulk" "$BULK_INSTALL_PATH"
  start_server "$SERVER_LOG_DIR/${tag}_server.log"
  for run in $(seq 1 "$NUM_RUNS_LONG"); do
    RUN_TAG="${tag}_r${run}" \
    MAIN_TRACE="$get_trace" LOOP_TRACE="$get_trace" \
    MAIN_OP=get LOOP_OP=get MAIN_ONE_OF=1 \
    NUM_INFLIGHT=$NUM_INFLIGHT_GET OPS_PER_THREAD=$OPS_PER_THREAD_GET \
      run_client || true
    read -r mops p50 p90 p99 <<<"$(parse_client_log "$CLIENT_LOG_DIR/${tag}_r${run}.log")"
    echo "$ds,$variant,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
  done
  stop_server
}

# sparse (artifact)
run_variant sparse learned 0 default "$TRACE_WORK_DIR/get.trace" "$BULK_TRACE"
run_variant sparse btree 1 inf "$TRACE_WORK_DIR/get.trace" "$BULK_TRACE"

# Extra datasets if present
for ds in sparseBig amzn osmc; do
  g=$TRACE_WORK_DIR/datasets/$ds/get.trace
  b=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  [ -f "$g" ] && [ -f "$b" ] || continue
  eps=default
  [ "$ds" = "osmc" ] && eps=16
  run_variant "$ds" learned 0 "$eps" "$g" "$b"
  run_variant "$ds" btree 1 inf "$g" "$b"
done

restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig 12 --data-dir "$OUT_DIR"
log "Fig.12 done → $CSV"
