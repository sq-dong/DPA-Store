#!/bin/bash
# memserver16 (DPA-Store server host).
#   sudo bash scripts/setup_dataplane.sh net
#   sudo bash scripts/setup_dataplane.sh nic-mode   # then cold power cycle if mlxfwreset times out
#   sudo bash scripts/setup_dataplane.sh traces
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
ACTION=${1:-net}
IFACE=ens5f1np1

case "$ACTION" in
  net)
    ip addr replace 10.0.1.12/24 dev "$IFACE"
    ip link set "$IFACE" up
    ip -br addr show "$IFACE"
    echo "Server dataplane ready. On 72: sudo bash $REPO/scripts/setup_dataplane_72.sh"
    ;;
  nic-mode)
    mst start || true
    if [ ! -e /dev/mst/mt41692_pciconf0 ]; then
      echo "No /dev/mst/mt41692_pciconf0"
      exit 1
    fi
    mlxconfig -d /dev/mst/mt41692_pciconf0 q | grep -iE 'INTERNAL_CPU|OFFLOAD|LINK_TYPE' || true
    echo "Setting INTERNAL_CPU_OFFLOAD_ENGINE=DISABLED (NIC mode)..."
    mlxconfig -d /dev/mst/mt41692_pciconf0 -y s INTERNAL_CPU_OFFLOAD_ENGINE=DISABLED
    echo "Check with: mlxconfig -d /dev/mst/mt41692_pciconf0 -e q | grep OFFLOAD"
    echo "Current must become DISABLED; if mlxfwreset times out, cold power-cycle the host."
    mlxfwreset -d /dev/mst/mt41692_pciconf0 -y -l 3 r \
      || echo "mlxfwreset failed — sudo poweroff, wait 30s, power on"
    ;;
  traces)
    bash "$SCRIPT_DIR/prepare_traces.sh" --install-tmp
    ;;
  *)
    echo "Usage: $0 {net|nic-mode|traces}"
    exit 1
    ;;
esac
