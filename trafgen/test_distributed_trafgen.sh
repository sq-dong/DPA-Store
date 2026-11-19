#!/bin/bash

# Distributed DPA-Store One-Way Ping Testing
# Runs multiple trafgen instances in parallel to distribute load across all reader threads
#
# CONFIGURE FOR YOUR SITE: this script bakes the lab MACs and IPs directly into
# the trafgen packet templates below.  To run on a non-lab setup, edit:
#   - INTERFACE                                  (this line — your NIC name)
#   - dst_mac (DPA-Store)  inside create_config_for_port()
#   - dst_mac (DPA client) inside create_config_for_port()
#   - "Src MAC (Client)"   inside the cat heredoc
#   - "Src IP" / "Dst IP"  inside the cat heredoc (currently 10.0.1.11/12)
# Or override INTERFACE at runtime with the -i flag.

# Local cluster default: Ethernet netdev on bfhost_72 (mlx5_1 → ens2f1np1).
# Override with -i if ibdev2netdev shows a different name.
INTERFACE="ens2f1np1"
READER_THREADS=176
# Must match common/dpastore_common_config.h: CLIENT + UDP_PORT_BUDGET(256) + 1
# Do NOT use READER_THREADS here — Fig.9 sweeps change N without moving the base.
UDP_PORT_BUDGET=256
BASE_PORT=$((23998 + UDP_PORT_BUDGET + 1))
TOTAL_PACKETS="$READER_THREADS"
PACKETS_PER_THREAD=$((TOTAL_PACKETS / READER_THREADS))
DPA_CLIENT_MODE=false

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
CONFIG_HEADER="$REPO_ROOT/common/dpastore_common_config.h"

if [ -n "$DPASTORE_RANGE_REQUEST" ]; then
    RANGE_ENABLED=$DPASTORE_RANGE_REQUEST
elif [ -f "$CONFIG_HEADER" ]; then
    if grep -Eq "^[[:space:]]*#define[[:space:]]+DPASTORE_RANGE_REQUEST[[:space:]]+0" "$CONFIG_HEADER"; then
        RANGE_ENABLED=0
    else
        RANGE_ENABLED=1
    fi
else
    RANGE_ENABLED=1
fi

if [ "$RANGE_ENABLED" -ne 0 ]; then
    RANGE_ENABLED=1
fi

BASE_PAYLOAD_BYTES=33
RANGE_ENTRY_COUNT=10
RANGE_ENTRY_SIZE=16
if [ "$RANGE_ENABLED" -eq 1 ]; then
    EXTRA_PAYLOAD_BYTES=$((RANGE_ENTRY_COUNT * RANGE_ENTRY_SIZE))
else
    EXTRA_PAYLOAD_BYTES=0
fi
PAYLOAD_BYTES=$((BASE_PAYLOAD_BYTES + EXTRA_PAYLOAD_BYTES))
IP_TOTAL_LEN=$((20 + 8 + PAYLOAD_BYTES))
UDP_TOTAL_LEN=$((8 + PAYLOAD_BYTES))
IP_TOTAL_LEN_HI=$((IP_TOTAL_LEN >> 8))
IP_TOTAL_LEN_LO=$((IP_TOTAL_LEN & 0xff))
UDP_TOTAL_LEN_HI=$((UDP_TOTAL_LEN >> 8))
UDP_TOTAL_LEN_LO=$((UDP_TOTAL_LEN & 0xff))

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_usage() {
    echo -e "${BLUE}DPA-Store Distributed One-Way Ping Testing${NC}"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -i INTERFACE    Network interface (default: $INTERFACE)"
    echo "  -n TOTAL_PKTS   Total packets to send across all threads (default: $TOTAL_PACKETS)"
    echo "  -r RATE         Packet rate per thread (e.g., 1000pps, 10kpps, 100kpps)"
    echo "  -t THREADS      Number of reader threads to target (default: $READER_THREADS)"
    echo "  -d              DPA client mode - switch MAC addresses for DPA client/store matching"
    echo "  -h              Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                          # Send 1M packets distributed across 16 threads"
    echo "  $0 -n 160000 -r 10kpps     # Send 160k packets at 10kpps per thread"
    echo "  $0 -t 8 -n 80000           # Use only first 8 reader threads"
    echo "  $0 -d -n 160000            # DPA client mode with switched MAC addresses"
    echo ""
}

# Parse arguments
RATE=""
while getopts "i:n:r:t:dh" opt; do
    case $opt in
        i) INTERFACE="$OPTARG" ;;
        n) TOTAL_PACKETS="$OPTARG" ;;
        r) RATE="$OPTARG" ;;
        t) READER_THREADS="$OPTARG" ;;
        d) DPA_CLIENT_MODE=true ;;
        h) print_usage; exit 0 ;;
        \?) echo "Invalid option -$OPTARG" >&2; print_usage; exit 1 ;;
    esac
done

# Recalculate packets per thread
PACKETS_PER_THREAD=$((TOTAL_PACKETS / READER_THREADS))

echo -e "${BLUE}=== Distributed Trafgen Test Configuration ===${NC}"
echo "Interface: $INTERFACE"
echo "Total packets: $TOTAL_PACKETS"
echo "Reader threads: $READER_THREADS"
echo "Packets per thread: $PACKETS_PER_THREAD"
echo "Port range: $BASE_PORT to $((BASE_PORT + READER_THREADS - 1))"
if [ "$DPA_CLIENT_MODE" = true ]; then
    echo "Mode: DPA Client (switched MAC addresses)"
else
    echo "Mode: Standard (original MAC addresses)"
fi
if [ -n "$RATE" ]; then
    echo "Rate per thread: $RATE"
fi
if [ "$RANGE_ENABLED" -eq 1 ]; then
    echo "Range payload: enabled (${PAYLOAD_BYTES} bytes payload, IP len $IP_TOTAL_LEN, UDP len $UDP_TOTAL_LEN)"
else
    echo "Range payload: disabled (${PAYLOAD_BYTES} bytes payload, IP len $IP_TOTAL_LEN, UDP len $UDP_TOTAL_LEN)"
fi
echo ""

# Function to create trafgen config for a specific port
create_config_for_port() {
    local port=$1
    local config_file="trafgen_port_${port}.cfg"

    local port_high=$((port >> 8))
    local port_low=$((port & 0xff))
    local thread_idx=$((port - BASE_PORT))

    # Site MACs (memserver16 server NIC / bfhost_72 client NIC / client DPU).
    # Keep in sync with common/dpastore_common_config.h (MS16_* / BF72_*).
    if [ "$DPA_CLIENT_MODE" = true ]; then
        # Dst = local DPU port MAC (triggers dpa_client TX loop); VERIFY after DPU mode
        local dst_mac="0x7c, 0x8c, 0x09, 0xbf, 0x34, 0x39"
    else
        # Dst = server host NIC MAC (memserver16 ens6f1np1)
        local dst_mac="0x7c, 0x8c, 0x09, 0xbf, 0x36, 0x73"
    fi

    cat > "$config_file" << EOF
{
  # Ethernet Header
  $dst_mac,  # Dst MAC
  0x7c, 0x8c, 0x09, 0xbf, 0x34, 0x39,  # Src MAC (Client bfhost_72 — VERIFY)
  0x08, 0x00,                          # EtherType: IPv4

  # IPv4 Header
  0x45, 0x00, 0x$(printf "%02x" $IP_TOTAL_LEN_HI), 0x$(printf "%02x" $IP_TOTAL_LEN_LO),  # Version, IHL, ToS, Total Length ($IP_TOTAL_LEN bytes)
  0x00, 0x01, 0x40, 0x00,              # ID, Flags, Fragment
  0x40, 0x11, 0x00, 0x00,              # TTL, Protocol (UDP), Checksum
  0x0a, 0x00, 0x01, 0x0b,              # Src IP 10.0.1.11 (bfhost_72)
  0x0a, 0x00, 0x01, 0x0c,              # Dst IP 10.0.1.12 (memserver16)

  # UDP Header
  0x5d, 0xbe,                          # Src Port: 23998
  0x$(printf "%02x" $port_high), 0x$(printf "%02x" $port_low),  # Dst Port: $port
  0x$(printf "%02x" $UDP_TOTAL_LEN_HI), 0x$(printf "%02x" $UDP_TOTAL_LEN_LO),  # UDP Length
  0x00, 0x00,                          # Checksum

  # DPAStoreClientPacket payload
  0x00,                                # control (GET)
  0x00,                                # reserved
  0x01, 0x00, 0x00, 0x00,              # request_id
  0x$(printf "%02x" $((thread_idx & 0xff))),
  0x$(printf "%02x" $(((thread_idx >> 8) & 0xff))),
  0x$(printf "%02x" $(((thread_idx >> 16) & 0xff))),
  0x$(printf "%02x" $(((thread_idx >> 24) & 0xff))),
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

  # --- HotCachePlan (3 bytes) ---
  0x00, 0x00, 0x00,
EOF

    if [ "$RANGE_ENABLED" -eq 1 ]; then
        cat >> "$config_file" << 'EOF'
  # --- Timestamp (8 bytes) ---
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
EOF
        cat >> "$config_file" << EOF
  # --- Range entries ($RANGE_ENTRY_COUNT * 16 bytes) ---
EOF
        local entry last_entry trailing
        for ((entry=0; entry<RANGE_ENTRY_COUNT; entry++)); do
            last_entry=$(( entry == RANGE_ENTRY_COUNT - 1 ))
            trailing=$([ "$last_entry" -eq 1 ] && echo "" || echo ",")
            {
                printf '  # Range entry %d (key)\n' "$entry"
                echo "  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,"
                printf '  # Range entry %d (value)\n' "$entry"
                echo "  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00$trailing"
            } >> "$config_file"
        done
    else
        cat >> "$config_file" << 'EOF'
  # --- Timestamp (8 bytes) ---
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
EOF
    fi

    echo "}" >> "$config_file"
    echo "$config_file"
}

# Function to run trafgen for a specific port
run_trafgen_for_port() {
    local port=$1
    local config_file=$2
    local thread_id=$((port - BASE_PORT))

    echo -e "${YELLOW}Starting trafgen for thread $thread_id (port $port)${NC}"

    local cmd="trafgen --dev $INTERFACE --conf $config_file --num $PACKETS_PER_THREAD"
    if [ -n "$RATE" ]; then
        cmd="$cmd --rate $RATE"
    fi

    # Run in background and capture PID
    $cmd &
    local pid=$!
    echo "Thread $thread_id (port $port): PID $pid"
    return $pid
}

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root${NC}"
    echo "Use: sudo $0 $*"
    exit 1
fi

# Create temp directory for configs
TEMP_DIR=$(mktemp -d)
echo "Using temp directory: $TEMP_DIR"
cd "$TEMP_DIR"

# Track spawned trafgen PIDs so they can be reaped on exit/interrupt
PIDS=()

# Kill any still-running trafgen instances and remove temp files on exit or Ctrl-C
cleanup() {
    trap - EXIT INT TERM
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null
    fi
    if [ -n "$TEMP_DIR" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Create configs and start trafgen instances
echo -e "${GREEN}Creating configurations and starting trafgen instances...${NC}"

for ((i=0; i<READER_THREADS; i++)); do
    port=$((BASE_PORT + i))
    config_file=$(create_config_for_port $port)
    run_trafgen_for_port $port "$config_file"
    PIDS+=($!)
done

echo ""
echo -e "${GREEN}All trafgen instances started. Waiting for completion...${NC}"
echo -e "${YELLOW}Press Ctrl+C on the DPA-Store server to see distributed throughput statistics${NC}"
echo ""

# Wait for all processes to complete
for pid in "${PIDS[@]}"; do
    wait $pid
    echo "Process $pid completed"
done

echo ""
echo -e "${GREEN}All trafgen instances completed!${NC}"
echo -e "${BLUE}Check DPA-Store server statistics - you should see packets distributed across all $READER_THREADS reader threads${NC}"

# Temp files and any stray trafgen instances are removed by the cleanup trap on exit
