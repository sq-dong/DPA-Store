#!/bin/bash
# Extract artifact traces into $REPO/traces/ and optionally install bulk to /tmp
# (server default path is /tmp/dpastore_bulk.trace).
#   bash scripts/prepare_traces.sh
#   bash scripts/prepare_traces.sh --install-tmp
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
TRACES="$REPO/traces"
INSTALL_TMP=0
[ "${1:-}" = "--install-tmp" ] && INSTALL_TMP=1

mkdir -p "$TRACES"
cd "$TRACES"

if [ ! -f read.trace ]; then
  if [ -f extracted/read.trace ]; then
    cp -f extracted/read.trace read.trace
  elif [ -f read_trace.tar.gz ]; then
    tar xzf read_trace.tar.gz
    # tarball may unpack as read.trace or nested path
    if [ ! -f read.trace ]; then
      found=$(find . -name 'read.trace' -type f | head -1)
      [ -n "$found" ] && cp -f "$found" read.trace
    fi
  else
    echo "No read.trace or read_trace.tar.gz in $TRACES" >&2
    exit 1
  fi
fi

if [ ! -f dpastore_bulk.trace ]; then
  if [ -f extracted/tmp/dpastore_bulk.trace ]; then
    cp -f extracted/tmp/dpastore_bulk.trace dpastore_bulk.trace
  elif [ -f bulk_trace.tar.gz ]; then
    tar xzf bulk_trace.tar.gz
    if [ ! -f dpastore_bulk.trace ]; then
      found=$(find . -name 'dpastore_bulk.trace' -type f | head -1)
      [ -n "$found" ] && cp -f "$found" dpastore_bulk.trace
    fi
  else
    echo "No dpastore_bulk.trace or bulk_trace.tar.gz in $TRACES" >&2
    exit 1
  fi
fi

ls -lah "$TRACES/read.trace" "$TRACES/dpastore_bulk.trace"

if [ "$INSTALL_TMP" -eq 1 ]; then
  cp -f "$TRACES/dpastore_bulk.trace" /tmp/dpastore_bulk.trace
  cp -f "$TRACES/read.trace" /tmp/read.trace
  ls -lah /tmp/dpastore_bulk.trace /tmp/read.trace
fi
