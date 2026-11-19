#!/bin/bash
# Full paper reproduction (2 repeats / point). Run from a real TTY:
#   bash scripts/paper_repro/run_full_paper_repro.sh
set -euo pipefail
REPO=/home/dongshuqi/DPA-Store/dpa-store_branch_default
cd "$REPO"
OUT="$REPO/scripts/paper_repro/out"
mkdir -p "$OUT"
LOG="$OUT/full_repro.log"
STATUS="$OUT/full_repro_status.txt"

export QUICK=0
export NUM_RUNS_LONG=2
export NUM_RUNS_SHORT=2
# Short-enough ops to finish inside healthy DPA window (see config.env comment).
export OPS_PER_THREAD_GET="${OPS_PER_THREAD_GET:-500000}"
export OPS_PER_THREAD_WRITE="${OPS_PER_THREAD_WRITE:-150000}"
export CLIENT_MAX_WALL_S="${CLIENT_MAX_WALL_S:-90}"
export FIG15_SKIP_RANGE="${FIG15_SKIP_RANGE:-0}"

echo "== $(date -Is) full paper repro start ==" | tee "$STATUS"
echo "runs_long=$NUM_RUNS_LONG runs_short=$NUM_RUNS_SHORT ops_get=$OPS_PER_THREAD_GET ops_write=$OPS_PER_THREAD_WRITE wall=${CLIENT_MAX_WALL_S}s" | tee -a "$STATUS"

echo "== unlock local sudo =="
sudo -v

MULTI="$REPO/x86_client/build/multiclient"
if ! ssh -o BatchMode=yes bfhost_72 "sudo -n '$MULTI' --help >/dev/null 2>&1"; then
  echo "== client NOPASSWD missing; installing =="
  bash "$REPO/scripts/paper_repro/setup_client_nopasswd_sudo.sh"
fi

# Keep local sudo alive
( while true; do sudo -n true 2>/dev/null || exit 0; sleep 50; done ) &
KEEP=$!
trap 'kill $KEEP 2>/dev/null || true' EXIT

# Order: prep → fig10 → fig11 → fig09 → table1 → fig12 → fig13 → fig15 → plot
TARGETS=(prep fig10 fig11 fig09 table1 fig12 fig13 fig15 plot)

{
  echo "FULL_REPRO_PID=$$"
  echo "STARTED_AT=$(date -Is)"
  echo "TARGETS=${TARGETS[*]}"
} >"$OUT/full_repro.meta"

for t in "${TARGETS[@]}"; do
  echo "== $(date -Is) BEGIN $t ==" | tee -a "$STATUS"
  echo "$t" >"$OUT/full_repro_current.txt"
  if bash "$REPO/scripts/paper_repro/run_all.sh" "$t"; then
    echo "== $(date -Is) DONE  $t OK ==" | tee -a "$STATUS"
  else
    echo "== $(date -Is) DONE  $t FAIL (continuing) ==" | tee -a "$STATUS"
  fi
done

echo "FINISHED_AT=$(date -Is)" >>"$OUT/full_repro.meta"
echo "idle" >"$OUT/full_repro_current.txt"
echo "== $(date -Is) ALL TARGETS FINISHED ==" | tee -a "$STATUS"
echo "CSVs/plots under $OUT"
