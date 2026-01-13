#!/usr/bin/env bash
set -e

NUM_VMS=$1
OUTDIR=$2

mkdir -p "$OUTDIR"

# Example: start server-side workload
echo "[node1] Starting test for $NUM_VMS VMs"

# example: iperf server, ovs stats, etc.
iperf3 -s -D

sleep 1

# collect stats (example)
ovs-appctl dpif-netdev/pmd-stats-show > "$OUTDIR/pmd.txt"

# wait until node0 finishes
sleep 10

pkill iperf3
echo "[node1] Done"
