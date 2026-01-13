#!/usr/bin/env bash
set -e

NODE1=node1
BASE_OUT=results
TEST_TIME=10

for VMS in $(seq 1 32); do
  echo "=============================="
  echo "Running test: $VMS VMs"
  echo "=============================="

  OUTDIR="$BASE_OUT/vms_$VMS"

  # start node1 first
  ssh "$NODE1" \
    "bash -s" < run_node1.sh "$VMS" "$OUTDIR"

  sleep 2

  # run node0 side
  bash run_node0.sh "$VMS" "$OUTDIR"

  # optional cooldown
  sleep 5
done
