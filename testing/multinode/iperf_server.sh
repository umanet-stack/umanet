#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOG_DIR="$SCRIPT_DIR/logs"
sudo rm -rf "$LOG_DIR"/*
sudo mkdir -p "$LOG_DIR"

for i in {0..31}; do
    PORT=$((5200 + i))
    sudo iperf3 -s -p $PORT > "$LOG_DIR/iperf_server_$i.log" 2>&1 &
    echo "  iperf3 server $i -> $PORT"
done

echo "✅  iperf3 servers started"