#!/bin/bash
# Fig.9 — traverser / patcher+stitcher scaling (paper §4.2.2).
# Left: GET, vary TRAVERSERS; Right: INSERT/UPDATE, vary stitcher/patcher at 176 trav.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

ensure_bulk_installed
require_file "$TRACE_WORK_DIR/get.trace"
require_file "$TRACE_WORK_DIR/update.trace"
require_file "$TRACE_WORK_DIR/insert.trace"

CSV_L="$OUT_DIR/fig09_traverser.csv"
CSV_R="$OUT_DIR/fig09_stitch_patch.csv"

PANEL=${1:-both}  # left | right | both

# Only truncate the CSV(s) for panels we are about to rewrite.
if [ "$PANEL" = "left" ] || [ "$PANEL" = "both" ]; then
  echo "traversers,run,mops,p50_us,p90_us,p99_us" >"$CSV_L"
fi
if [ "$PANEL" = "right" ] || [ "$PANEL" = "both" ]; then
  echo "stitchers,patchers,op,run,mops,p50_us,p90_us,p99_us" >"$CSV_R"
fi

# ---- left: traverser sweep (GET) ----
if [ "$PANEL" = "left" ] || [ "$PANEL" = "both" ]; then
  # 188 exceeds BF-3 DPA thread budget (create_thread fails); paper max ≈186.
  TRAVS=(1 2 4 8 16 32 64 128 176)
  [ "$QUICK" = "1" ] && TRAVS=(8 32 176)

  for n in "${TRAVS[@]}"; do
    rebuild_server TRAVERSERS=$n PATCHERS=4 STITCHERS=4 HOT_CACHE=1 EPSILON_MODE=default BTREE=0
    start_server "$SERVER_LOG_DIR/fig09_trav${n}.log"
    for run in $(seq 1 "$NUM_RUNS_LONG"); do
      RUN_TAG="fig09_trav${n}_r${run}" \
      MAIN_TRACE="$TRACE_WORK_DIR/get.trace" LOOP_TRACE="$TRACE_WORK_DIR/get.trace" \
      MAIN_OP=get LOOP_OP=get MAIN_ONE_OF=1 \
      NUM_INFLIGHT=$NUM_INFLIGHT_GET OPS_PER_THREAD=$OPS_PER_THREAD_GET \
      NUM_SERVER_READERS=$n \
        run_client || true
      if metrics=$(parse_client_log "$CLIENT_LOG_DIR/fig09_trav${n}_r${run}.log"); then
        read -r mops p50 p90 p99 <<<"$metrics"
        echo "$n,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV_L"
      else
        log "WARN: parse failed fig09 trav=$n run=$run"
        echo "$n,$run,nan,nan,nan,nan" | tee -a "$CSV_L"
      fi
    done
    stop_server || true
  done
fi

# ---- right: stitcher/patcher sweep (UPDATE + INSERT) at 176 trav ----
if [ "$PANEL" = "right" ] || [ "$PANEL" = "both" ]; then
  # Labels in paper: 1s/4p, 2s/4p, 4s/1p, 4s/2p, 4s/4p
  CFGS=("1:4" "2:4" "4:1" "4:2" "4:4")
  [ "$QUICK" = "1" ] && CFGS=("1:4" "4:4")

  for cfg in "${CFGS[@]}"; do
    s=${cfg%%:*}; p=${cfg##*:}
    rebuild_server TRAVERSERS=176 PATCHERS=$p STITCHERS=$s HOT_CACHE=1 EPSILON_MODE=default BTREE=0
    for op in update insert; do
      trace=$TRACE_WORK_DIR/update.trace
      [ "$op" = "insert" ] && trace=$TRACE_WORK_DIR/insert.trace
      runs=$NUM_RUNS_SHORT
      [ "$op" = "update" ] && runs=$NUM_RUNS_LONG
      for run in $(seq 1 "$runs"); do
        # WRITE runs kill DPA after ~1 invocation; fresh server every run.
        start_server "$SERVER_LOG_DIR/fig09_s${s}_p${p}_${op}_r${run}_server.log"
        RUN_TAG="fig09_s${s}_p${p}_${op}_r${run}" \
        MAIN_TRACE="$trace" LOOP_TRACE="$trace" \
        MAIN_OP=$op LOOP_OP=$op MAIN_ONE_OF=1 \
        NUM_INFLIGHT=$NUM_INFLIGHT_WRITE OPS_PER_THREAD=$OPS_PER_THREAD_WRITE \
        NUM_SERVER_READERS=176 \
          run_client || true
        if metrics=$(parse_client_log "$CLIENT_LOG_DIR/fig09_s${s}_p${p}_${op}_r${run}.log"); then
          read -r mops p50 p90 p99 <<<"$metrics"
          echo "$s,$p,$op,$run,$mops,$p50,$p90,$p99" | tee -a "$CSV_R"
        else
          log "WARN: parse failed fig09 s=$s p=$p op=$op run=$run"
          echo "$s,$p,$op,$run,nan,nan,nan,nan" | tee -a "$CSV_R"
        fi
        stop_server || true
      done
    done
  done
fi

restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig 9 --data-dir "$OUT_DIR"
log "Fig.9 done"
