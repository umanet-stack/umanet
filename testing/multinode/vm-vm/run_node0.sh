#!/usr/bin/env bash
set -e

NUM_VMS=$1
OUTDIR=$2

mkdir -p "$OUTDIR"

echo "[node0] Running client with $NUM_VMS VMs"

# example client-side load
iperf3 -c node1 -P "$NUM_VMS" -t 10 > "$OUTDIR/iperf.txt"

# collect local stats
ovs-appctl dpif-netdev/pmd-stats-show > "$OUTDIR/pmd.txt"

echo "[node0] Done"
