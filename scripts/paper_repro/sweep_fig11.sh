#!/bin/bash
# Fig.11 — GET × dataset / ε / uniform vs Zipf (paper §4.2.4).
# With artifact traces only `sparse` is available locally. Extra datasets
# (amzn/osmc/face/…) can be dropped into $TRACE_WORK_DIR/datasets/<name>/{bulk,get}.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"
require_file "$TRACE_WORK_DIR/zipf_get.trace"

CSV="$OUT_DIR/fig11_get_datasets.csv"
echo "dataset,depth_label,eps_label,dist,hot_cache,run,mops,p50_us,p90_us,p99_us" >"$CSV"

# Default sparse (artifact): depth≈3, ε=4/8
run_one() {
  local ds=$1 depth=$2 eps_lbl=$3 dist=$4 hot=$5 eps_mode=$6
  local get_trace=$7
  local tag="fig11_${ds}_${dist}_hot${hot}"
  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=$hot \
    EPSILON_MODE=$eps_mode BTREE=0
  # If a dataset-specific bulk is provided, install it
  local bulk_override=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  if [ -f "$bulk_override" ]; then
    sudo cp -f "$bulk_override" "$BULK_INSTALL_PATH"
  else
    ensure_bulk_installed
  fi
  for run in $(seq 1 "$NUM_RUNS_LONG"); do
    start_server "$SERVER_LOG_DIR/${tag}_r${run}_server.log"
    RUN_TAG="${tag}_r${run}" \
    MAIN_TRACE="$get_trace" LOOP_TRACE="$get_trace" \
    MAIN_OP=get LOOP_OP=get MAIN_ONE_OF=1 \
    NUM_INFLIGHT=$NUM_INFLIGHT_GET OPS_PER_THREAD=$OPS_PER_THREAD_GET \
      run_client || true
    if metrics=$(parse_client_log "$CLIENT_LOG_DIR/${tag}_r${run}.log"); then
      read -r mops p50 p90 p99 <<<"$metrics"
      echo "$ds,$depth,$eps_lbl,$dist,$hot,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV"
    else
      log "WARN: parse failed for $tag run=$run"
      echo "$ds,$depth,$eps_lbl,$dist,$hot,$run,nan,nan,nan,nan" | tee -a "$CSV"
    fi
    stop_server || true
  done
}

# sparse uniform / zipf, hot cache on (paper default)
run_one sparse d3 e4/8 uniform 1 default "$TRACE_WORK_DIR/get.trace"
run_one sparse d3 e4/8 zipf 1 default "$TRACE_WORK_DIR/zipf_get.trace"

# Optional: hot-cache off contrast (mentioned in §4.2.4 lessons)
if [ "${FIG11_HOTCACHE_OFF:-0}" = "1" ]; then
  run_one sparse d3 e4/8 zipf 0 default "$TRACE_WORK_DIR/zipf_get.trace"
fi

# Extra SOSD datasets if present
for ds in dense4x amzn wiki sparseBig face osmc; do
  g=$TRACE_WORK_DIR/datasets/$ds/get.trace
  b=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  [ -f "$g" ] && [ -f "$b" ] || continue
  eps=default; eps_lbl=e4/8; depth=d3
  case "$ds" in
    face|osmc) eps=16; eps_lbl=e16/16; depth=d3 ;;
    sparseBig|amzn|wiki|dense4x) depth=d4 ;;
  esac
  z=$TRACE_WORK_DIR/datasets/$ds/zipf_get.trace
  run_one "$ds" "$depth" "$eps_lbl" uniform 1 "$eps" "$g"
  if [ -f "$z" ]; then
    run_one "$ds" "$depth" "$eps_lbl" zipf 1 "$eps" "$z"
  fi
done

restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig 11 --data-dir "$OUT_DIR"
log "Fig.11 done → $CSV"
