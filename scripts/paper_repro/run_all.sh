#!/bin/bash
# Orchestrate all reproducible paper figures for this cluster.
#
# Usage:
#   bash scripts/paper_repro/run_all.sh              # full paper-scale
#   QUICK=1 bash scripts/paper_repro/run_all.sh      # wiring / short ops
#   bash scripts/paper_repro/run_all.sh fig10 fig13  # subset
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

TARGETS=("${@}")
if [ ${#TARGETS[@]} -eq 0 ]; then
  # Recommended order from docs/DPA-Store-experiments.md §5
  TARGETS=(prep fig10 fig11 fig09 table1 fig12 fig13 fig15 plot)
fi

do_prep() {
  log "=== prepare traces + sync client ==="
  bash "$REPO/scripts/prepare_traces.sh" --install-tmp
  python3 "$DIR/prepare_workload_traces.py" \
    --bulk "$BULK_TRACE" \
    --read "$READ_TRACE_SRC" \
    --out-dir "$TRACE_WORK_DIR" \
    --zipf-alpha "$ZIPF_ALPHA"
  sync_repo_to_client
  if ! ssh -o BatchMode=yes "$CLIENT_SSH" "test -x $CLIENT_BIN_REMOTE"; then
    build_client_remote
  fi
  # push generated traces
  rsync -az "$TRACE_WORK_DIR/" "$CLIENT_SSH:$TRACE_WORK_DIR/"
}

do_plot() {
  python3 "$DIR/plot_paper.py" --fig all --data-dir "$OUT_DIR"
  log "figures under $OUT_DIR/*.png"
}

for t in "${TARGETS[@]}"; do
  case "$t" in
    prep) do_prep ;;
    fig9|fig09) bash "$DIR/sweep_fig09.sh" ;;
    fig10) bash "$DIR/sweep_fig10.sh" ;;
    fig11) bash "$DIR/sweep_fig11.sh" ;;
    fig12) bash "$DIR/sweep_fig12.sh" ;;
    fig13) bash "$DIR/sweep_fig13.sh" ;;
    fig15) bash "$DIR/sweep_fig15.sh" ;;
    table1) bash "$DIR/table1.sh" ;;
    plot) do_plot ;;
    *) die "unknown target: $t (prep|fig09|fig10|fig11|fig12|fig13|fig15|table1|plot)" ;;
  esac
done

log "all requested targets done"
