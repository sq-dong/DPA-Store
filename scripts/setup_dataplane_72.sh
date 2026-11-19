#!/bin/bash
# bfhost_72 / cpuserver72 (DPDK multiclient host).
#   sudo bash scripts/setup_dataplane_72.sh
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
IFACE=ens2f1np1

ip link set "$IFACE" up
ip addr replace 10.0.1.11/24 dev "$IFACE"
ip -br addr show "$IFACE"
echo "carrier=$(cat /sys/class/net/$IFACE/carrier 2>/dev/null || echo n/a) oper=$(cat /sys/class/net/$IFACE/operstate)"
ping -c 3 -W 2 10.0.1.12 || true
arping -I "$IFACE" -c 3 10.0.1.12 2>/dev/null || true

# Ensure read.trace exists under repo (client smoke uses TRACE_DIR=$REPO/traces)
if [ ! -f "$REPO/traces/read.trace" ]; then
  bash "$SCRIPT_DIR/prepare_traces.sh" || true
fi
ls -lah "$REPO/traces/read.trace" 2>/dev/null || true
