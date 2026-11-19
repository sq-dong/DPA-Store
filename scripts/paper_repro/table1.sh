#!/bin/bash
# Table 1 — index memory overhead after bulk-load (paper §4.2.1).
# Parses server 't' tree info: host/DPA pool usage vs raw KV size.
# Paper uses 50M keys; artifact sparse bulk is 25M — we report both absolute
# MB and relative overhead, and label N.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
source "$DIR/lib.sh"

CSV="$OUT_DIR/table1_memory.csv"
echo "dataset,n_keys,eps,host_leaf_mb,host_inner_mb,dpa_leaf_mb,dpa_inner_mb,dpa_total_mb,raw_kv_mb,rel_overhead_pct" >"$CSV"

measure_one() {
  local ds=$1 bulk=$2 eps=$3
  rebuild_server TRAVERSERS=176 PATCHERS=4 STITCHERS=4 HOT_CACHE=1 \
    EPSILON_MODE=$eps BTREE=0
  sudo cp -f "$bulk" "$BULK_INSTALL_PATH"
  local logf="$SERVER_LOG_DIR/table1_${ds}_eps${eps}.log"
  start_server "$logf"
  sleep 2
  printf 't' >&9 || true
  sleep 2
  python3 - "$logf" "$ds" "$eps" "$CSV" <<'PY'
import re, sys
logf, ds, eps, csv = sys.argv[1:5]
text = open(logf, errors="replace").read()
# take the last Tree Information block
blocks = text.split("=== Tree Information ===")
if len(blocks) < 2:
    raise SystemExit("no Tree Information in " + logf)
block = blocks[-1]
def grab(pat):
    m = re.search(pat, block)
    return int(m.group(1)) if m else 0
entries = grab(r"Total entries:\s*(\d+)")
host_leaf = grab(r"Leaf pool usage:\s*(\d+)\s*MB")
# two Leaf pool usage lines (host then DPA) — parse more carefully
host = re.search(r"Host pools:.*?Leaf pool usage:\s*(\d+)\s*MB.*?Inner pool usage:\s*(\d+)\s*MB",
                 block, re.S)
dpa = re.search(r"DPA pools:.*?Leaf pool usage:\s*(\d+)\s*MB.*?Inner pool usage:\s*(\d+)\s*MB",
                block, re.S)
hl = int(host.group(1)) if host else 0
hi = int(host.group(2)) if host else 0
dl = int(dpa.group(1)) if dpa else 0
di = int(dpa.group(2)) if dpa else 0
dpa_total = dl + di
raw = entries * 16 / (1024 * 1024)  # 8B key + 8B value
host_idx = hl + hi
rel = (host_idx / raw * 100.0) if raw > 0 else float("nan")
line = f"{ds},{entries},{eps},{hl},{hi},{dl},{di},{dpa_total},{raw:.2f},{rel:.1f}"
print(line)
open(csv, "a").write(line + "\n")
PY
  stop_server
}

ensure_bulk_installed
measure_one sparse "$BULK_TRACE" default

# ε=16 contrast on same sparse (paper shows this for face/osmc; still useful)
if [ "${TABLE1_EPS16:-0}" = "1" ]; then
  measure_one sparse "$BULK_TRACE" 16
fi

for ds in dense4x wiki amzn osmc face sparseBig; do
  b=$TRACE_WORK_DIR/datasets/$ds/bulk.trace
  [ -f "$b" ] || continue
  eps=default
  case "$ds" in face|osmc) eps=16 ;; esac
  measure_one "$ds" "$b" "$eps"
  if [ "$ds" = "face" ] || [ "$ds" = "osmc" ]; then
    # also default ε for comparison like Table 1
    measure_one "$ds" "$b" default
  fi
done

restore_server_defaults || true
python3 "$DIR/plot_paper.py" --fig table1 --data-dir "$OUT_DIR"
log "Table 1 done → $CSV"
