#!/bin/bash

echo "Checking trafgen installation and syntax..."
echo ""

# Check if trafgen is installed
if ! command -v trafgen &> /dev/null; then
    echo "ERROR: trafgen is not installed"
    echo "Install with: sudo apt-get install netsniff-ng"
    exit 1
fi

# Check trafgen version and help
echo "=== Trafgen Version ==="
trafgen --version 2>/dev/null || trafgen -v 2>/dev/null || echo "Version info not available"
echo ""

echo "=== Trafgen Help (first 30 lines) ==="
trafgen --help 2>/dev/null | head -30 || trafgen -h 2>/dev/null | head -30

echo ""
echo "=== Interface Check ==="
echo "Available network interfaces:"
ip link show | grep -E '^[0-9]+:' | cut -d: -f2 | tr -d ' ' | head -10
