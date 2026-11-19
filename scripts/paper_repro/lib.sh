#!/bin/bash
# Shared helpers for paper_repro sweeps. Source after config.env.
set -euo pipefail

_paper_repro_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
source "$_paper_repro_dir/config.env"

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

require_file() { [ -f "$1" ] || die "missing file: $1"; }
require_bin() { [ -x "$1" ] || die "missing executable: $1"; }

dpdk_lcore_arg() {
  local n=${1:-$NUM_SHARD_THREADS}
  local end=${DPDK_LCORE_END:-$n}
  echo "0-${end}"
}

# ---------- server lifecycle (local) ----------

server_pidfile() { echo "$OUT_DIR/dpastore_host.pid"; }
server_fifo() { echo "$OUT_DIR/dpastore_host.stdin"; }
server_logfile() { echo "${1:-$SERVER_LOG_DIR/server.current.log}"; }

stop_server() {
  local pf pid holder
  pf=$(server_pidfile)
  if [ -f "$pf" ]; then
    pid=$(cat "$pf" || true)
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
      log "stopping server pid=$pid"
      printf 'q' >&9 2>/dev/null || printf 'q' >"$(server_fifo)" 2>/dev/null || true
      sleep 2
      kill "$pid" 2>/dev/null || true
      sleep 1
      sudo kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pf"
  fi
  holder="$OUT_DIR/dpastore_host.fifo_holder.pid"
  if [ -f "$holder" ]; then
    kill "$(cat "$holder")" 2>/dev/null || true
    rm -f "$holder"
  fi
  # Close our writer FD if open
  exec 9>&- 2>/dev/null || true
  sudo pkill -f 'dpastore_host' 2>/dev/null || true
  sleep 1
  rm -f "$(server_fifo)"
}

start_server() {
  local logf
  logf=$(server_logfile "${1:-}")
  require_bin "$SERVER_BIN"
  require_file "$BULK_INSTALL_PATH"
  stop_server

  local fifo holder_pid
  fifo=$(server_fifo)
  rm -f "$fifo"
  mkfifo "$fifo"

  # Keep a writer open so the server never blocks on open(stdin).
  # The holder process just sleeps until we kill it in stop_server.
  sleep infinity >"$fifo" &
  holder_pid=$!
  echo "$holder_pid" >"$OUT_DIR/dpastore_host.fifo_holder.pid"

  log "starting server → $logf"
  export PKG_CONFIG_PATH="$PKG_CONFIG_PATH_SERVER:${PKG_CONFIG_PATH:-}"
  (
    cd "$REPO/server"
    # sudo may drop FD inheritance; reopen fifo explicitly.
    sudo -E ./build/dpastore_host <"$fifo" >"$logf" 2>&1
  ) &
  echo $! >"$(server_pidfile)"

  # Also open FD 9 in this shell for sending 't'/'q'
  exec 9>"$fifo"

  wait_server_ready "$logf"
}

wait_server_ready() {
  local logf=$1
  local timeout=${SERVER_READY_TIMEOUT:-900}
  local t=0
  log "waiting for bulk-load ready (timeout ${timeout}s)…"
  while [ "$t" -lt "$timeout" ]; do
    if grep -q "Press 't' for tree info" "$logf" 2>/dev/null; then
      log "server ready after ${t}s"
      # dump tree info once for Table-1 style metrics
      printf 't' >&9 || true
      sleep 1
      return 0
    fi
    if ! kill -0 "$(cat "$(server_pidfile)" 2>/dev/null || echo 0)" 2>/dev/null; then
      tail -n 40 "$logf" >&2 || true
      die "server exited before ready; see $logf"
    fi
    sleep 2
    t=$((t + 2))
  done
  tail -n 80 "$logf" >&2 || true
  die "server ready timeout; see $logf"
}

server_send() {
  # send one stdin command char to running server
  printf '%s' "$1" >&9 || die "server stdin fifo not open"
}

# ---------- rebuild server with compile-time knobs ----------

# Usage: rebuild_server KEY=VAL KEY=VAL ...
# Recognized keys: TRAVERSERS PATCHERS STITCHERS HOT_CACHE RANGE EPSILON_MODE BTREE
#   EPSILON_MODE=default|16|inf
#   BTREE=0|1  → USE_BTREE_ALGORITHM
rebuild_server() {
  local traversers=$DEFAULT_TRAVERSERS
  local patchers=$DEFAULT_PATCHERS
  local stitchers=$DEFAULT_STITCHERS
  local hot_cache=1
  local range=0
  local epsilon_mode=default
  local btree=0

  for kv in "$@"; do
    case "$kv" in
      TRAVERSERS=*) traversers=${kv#*=} ;;
      PATCHERS=*) patchers=${kv#*=} ;;
      STITCHERS=*) stitchers=${kv#*=} ;;
      HOT_CACHE=*) hot_cache=${kv#*=} ;;
      RANGE=*) range=${kv#*=} ;;
      EPSILON_MODE=*) epsilon_mode=${kv#*=} ;;
      BTREE=*) btree=${kv#*=} ;;
      *) die "unknown rebuild knob: $kv" ;;
    esac
  done

  log "rebuild_server traversers=$traversers patchers=$patchers stitchers=$stitchers hot=$hot_cache range=$range eps=$epsilon_mode btree=$btree"

  # Patch common + server configs via Python for idempotent edits
  python3 - "$COMMON_CFG" "$SERVER_CFG" "$traversers" "$hot_cache" "$range" "$patchers" "$stitchers" <<'PY'
import re, sys
common, server, trav, hot, rng, patchers, stitchers = sys.argv[1:8]

def set_define(text, name, val, after_ifndef=False):
    # Remove any previously forced bare defines of name that we inserted
    # (keep the canonical #ifndef-wrapped or plain #define).
    text2 = text
    if re.search(rf'(?m)^#define {name}\s+\S+', text2):
        text2 = re.sub(rf'(?m)^#define {name}\s+\S+', f'#define {name} {val}', text2, count=1)
        # If ifndef-wrapped, the first #define inside may now be wrong — also set inside
        return text2
    if f'#ifndef {name}' in text2:
        text2 = text2.replace(f'#ifndef {name}', f'#define {name} {val}\n#ifndef {name}', 1)
        return text2
    return text2 + f'\n#define {name} {val}\n'

c = open(common).read()
c = set_define(c, 'DPASTORE_READER_THREAD_COUNT', trav)
c = set_define(c, 'DPASTORE_ENABLE_HOT_CACHE', hot)
# RANGE: force value both as leading define and inside ifndef body
c = re.sub(r'(?m)^#define DPASTORE_RANGE_REQUEST\s+\S+', f'#define DPASTORE_RANGE_REQUEST {rng}', c)
if '#define DPASTORE_RANGE_REQUEST' not in c:
    c = c.replace('#ifndef DPASTORE_RANGE_REQUEST',
                  f'#define DPASTORE_RANGE_REQUEST {rng}\n#ifndef DPASTORE_RANGE_REQUEST', 1)
open(common, 'w').write(c)

s = open(server).read()
# Strip previously inserted forced defines before ifndef to avoid stacking
for name in ('DPASTORE_PATCHER_THREAD_COUNT', 'DPASTORE_STITCHER_THREAD_COUNT'):
    # remove lines that are immediate forced overrides we added
    s = re.sub(rf'(?m)^#define {name} \d+\n(?=#ifndef {name})', '', s)
for name, val in (('DPASTORE_PATCHER_THREAD_COUNT', patchers),
                  ('DPASTORE_STITCHER_THREAD_COUNT', stitchers)):
    if re.search(rf'(?m)^#define {name}\s+\d+', s) and f'#ifndef {name}' not in s:
        s = re.sub(rf'(?m)^#define {name}\s+\d+', f'#define {name} {val}', s)
    elif f'#ifndef {name}' in s:
        s = s.replace(f'#ifndef {name}', f'#define {name} {val}\n#ifndef {name}', 1)
    else:
        s += f'\n#define {name} {val}\n'
open(server, 'w').write(s)
PY

  # Epsilon modes in tree_config.h
  python3 - "$TREE_CFG" "$epsilon_mode" <<'PY'
import re, sys
path, mode = sys.argv[1], sys.argv[2]
text = open(path).read()
# Uncomment / set the active epsilon block.
def set_eps(inner, leaf, text):
    reps = {
        'EPSILON_BUILD_BULKLOAD_INNER': inner,
        'EPSILON_BUILD_BULKLOAD_LEAF': leaf,
        'EPSILON_BUILD_RUNTIME_INNER': inner,
        'EPSILON_BUILD_RUNTIME_LEAF': leaf,
        'EPSILON_TRAVERSE_INNER': inner,
        'EPSILON_TRAVERSE_LEAF': leaf,
    }
    for k, v in reps.items():
        text = re.sub(rf'(?m)^//\s*#define {k}\s+\d+', f'#define {k} {v}', text)
        text = re.sub(rf'(?m)^#define {k}\s+\d+', f'#define {k} {v}', text)
    return text

if mode == 'default':
    text = set_eps(4, 8, text)
elif mode == '16':
    text = set_eps(16, 16, text)
elif mode == 'inf':
    # Large epsilon → packed nodes (B+-like build); pair with USE_BTREE_ALGORITHM
    text = set_eps(1000000, 1000000, text)
else:
    raise SystemExit(f'bad EPSILON_MODE={mode}')
open(path, 'w').write(text)
PY

  # B-tree traversal algorithm
  python3 - "$DPA_OPS_H" "$btree" <<'PY'
import re, sys
path, btree = sys.argv[1], sys.argv[2]
text = open(path).read()
if btree == '1':
    text = re.sub(r'(?m)^//\s*#define USE_BTREE_ALGORITHM\s*$', '#define USE_BTREE_ALGORITHM', text)
    text = re.sub(r'(?m)^//#define USE_BTREE_ALGORITHM\s*$', '#define USE_BTREE_ALGORITHM', text)
else:
    # comment out active define, keep at most one commented form
    text = re.sub(r'(?m)^#define USE_BTREE_ALGORITHM\s*$', '//#define USE_BTREE_ALGORITHM', text)
open(path, 'w').write(text)
PY

  export PKG_CONFIG_PATH="$PKG_CONFIG_PATH_SERVER:${PKG_CONFIG_PATH:-}"
  export PATH="/opt/mellanox/doca/tools:${PATH:-}"
  chmod +x "$REPO/third_party/dpacc/build_dpacc.sh" 2>/dev/null || true
  cd "$REPO/server"
  cmake -S . -B build >/dev/null
  cmake --build build -j"$(nproc)"
  require_bin "$SERVER_BIN"
  log "rebuild done: $SERVER_BIN"
}

restore_server_defaults() {
  rebuild_server \
    TRAVERSERS=$DEFAULT_TRAVERSERS \
    PATCHERS=$DEFAULT_PATCHERS \
    STITCHERS=$DEFAULT_STITCHERS \
    HOT_CACHE=1 \
    RANGE=0 \
    EPSILON_MODE=default \
    BTREE=0
}

# ---------- client (remote via ssh) ----------

run_client() {
  # Args via env:
  #   RUN_TAG, MAIN_TRACE, LOOP_TRACE, MAIN_OP, LOOP_OP, MAIN_ONE_OF,
  #   NUM_INFLIGHT, OPS_PER_THREAD, NUM_SHARD_THREADS
  local tag=${RUN_TAG:-run}
  local main_trace=${MAIN_TRACE:-$TRACE_WORK_DIR/get.trace}
  local loop_trace=${LOOP_TRACE:-$main_trace}
  local main_op=${MAIN_OP:-get}
  local loop_op=${LOOP_OP:-get}
  local main_one_of=${MAIN_ONE_OF:-1}
  local inflight=${NUM_INFLIGHT:-$NUM_INFLIGHT_GET}
  local ops=${OPS_PER_THREAD:-$OPS_PER_THREAD_GET}
  local threads=${NUM_SHARD_THREADS}
  # Must match server DPASTORE_READER_THREAD_COUNT for the running binary.
  local readers=${NUM_SERVER_READERS:-$DEFAULT_TRAVERSERS}
  local logf="$CLIENT_LOG_DIR/${tag}.log"
  local lcores
  lcores=$(dpdk_lcore_arg "$threads")

  # Ensure remote has traces + binary
  ssh -o BatchMode=yes "$CLIENT_SSH" "test -x $CLIENT_BIN_REMOTE" \
    || die "remote multiclient missing: $CLIENT_BIN_REMOTE (rsync/build first)"

  # Sync the specific traces used for this run
  ssh -o BatchMode=yes "$CLIENT_SSH" "mkdir -p $TRACE_WORK_DIR $CLIENT_LOG_DIR"
  rsync -az "$main_trace" "$CLIENT_SSH:$main_trace"
  if [ "$loop_trace" != "$main_trace" ]; then
    rsync -az "$loop_trace" "$CLIENT_SSH:$loop_trace"
  fi

  local wall=${CLIENT_MAX_WALL_S:-0}
  log "client $tag threads=$threads readers=$readers inflight=$inflight ops=$ops wall=${wall}s main=$main_op/$main_one_of"
  # DPDK args before -- ; gflags after --
  # Absolute path required to match sudoers NOPASSWD rule.
  # Install once via scripts/paper_repro/setup_client_nopasswd_sudo.sh
  # Wall-clock guard: if DPA dies mid-run, multiclient can spin on timeouts for a long time.
  ssh -o BatchMode=yes "$CLIENT_SSH" bash -s <<EOF >"$logf" 2>&1
set -euo pipefail
cd "$REPO/x86_client"
cmd=(sudo -n $CLIENT_BIN_REMOTE -l $lcores -a $DEV_CLIENT_PCI -- \
  -dst_ip=$SERVER_IP -dst_mac=$SERVER_MAC \
  -src_ip=$CLIENT_IP -src_mac=$CLIENT_MAC \
  -dev_name=$DEV_CLIENT_PCI \
  -main_trace_path=$main_trace -main_trace_op=$main_op \
  -loop_trace_path=$loop_trace -loop_trace_op=$loop_op \
  -main_one_of=$main_one_of -trace_wrap_main=true \
  -num_shard_threads=$threads -num_inflight=$inflight \
  -num_server_readers=$readers \
  -ops_per_thread=$ops -starvation_time=$STARVATION_TIME_US)
# Do NOT use sudo -E: NOPASSWD rule has no SETENV.
if [ "$wall" -gt 0 ] 2>/dev/null; then
  "\${cmd[@]}" &
  cpid=\$!
  (
    sleep "$wall"
    # Best-effort: NOPASSWD may only cover multiclient itself; try both.
    kill "\$cpid" 2>/dev/null || true
    sudo -n kill "\$cpid" 2>/dev/null || true
    sudo -n kill -9 "\$cpid" 2>/dev/null || true
    pkill -f "$CLIENT_BIN_REMOTE" 2>/dev/null || true
    sudo -n pkill -9 -f "$CLIENT_BIN_REMOTE" 2>/dev/null || true
  ) &
  wpid=\$!
  set +e
  wait "\$cpid"
  rc=\$?
  set -e
  kill "\$wpid" 2>/dev/null || true
  wait "\$wpid" 2>/dev/null || true
  exit "\$rc"
else
  "\${cmd[@]}"
fi
EOF
  local rc=$?
  log "client $tag finished rc=$rc → $logf"
  return $rc
}

parse_client_log() {
  # stdout: mops,p50_us,p90_us,p99_us  (non-zero exit on parse failure)
  local out
  if ! out=$(python3 "$_paper_repro_dir/parse_metrics.py" "$1" \
      --trim-head "$TRIM_HEAD_S" --trim-tail "$TRIM_TAIL_S"); then
    return 1
  fi
  printf '%s\n' "$out"
}

ensure_bulk_installed() {
  require_file "$BULK_TRACE"
  if [ ! -f "$BULK_INSTALL_PATH" ] || ! cmp -s "$BULK_TRACE" "$BULK_INSTALL_PATH" 2>/dev/null; then
    log "installing bulk trace → $BULK_INSTALL_PATH"
    sudo cp -f "$BULK_TRACE" "$BULK_INSTALL_PATH"
  fi
}

sync_repo_to_client() {
  log "rsync repo → $CLIENT_SSH (excluding builds/tarballs)"
  ssh -o BatchMode=yes "$CLIENT_SSH" "mkdir -p $REPO"
  rsync -az --delete \
    --exclude '**/build*' \
    --exclude 'traces/*.tar.gz' \
    --exclude '**/.git' \
    --exclude 'scripts/paper_repro/out' \
    "$REPO/" "$CLIENT_SSH:$REPO/"
  # Always push client binary if present locally; otherwise build remotely.
  if [ -x "$REPO/x86_client/build/multiclient" ]; then
    ssh -o BatchMode=yes "$CLIENT_SSH" "mkdir -p $REPO/x86_client/build"
    rsync -az "$REPO/x86_client/build/multiclient" "$CLIENT_SSH:$REPO/x86_client/build/multiclient"
  fi
}

build_client_remote() {
  log "build multiclient on $CLIENT_SSH"
  ssh -o BatchMode=yes "$CLIENT_SSH" bash -s <<EOF
set -euo pipefail
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH_CLIENT:\${PKG_CONFIG_PATH:-}"
cd "$REPO/x86_client"
cmake -S . -B build -DMULTICLIENT_TARGET=DPDK
cmake --build build -j\$(nproc)
test -x build/multiclient
EOF
}
