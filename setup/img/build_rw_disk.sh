#!/usr/bin/env bash
set -ex

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <num_disks> <size>"
  echo "  num_vms: number of disks to build"
  echo "  size: size of each disk in MB"
  echo "Example: $0 32 512"
  exit 1
fi

NUM_DISKS=$1
SIZE=$2
SCRIPT_DIR="/tmp/disks"

rm -rf "$SCRIPT_DIR"
mkdir -p "$SCRIPT_DIR"

for i in $(seq 0 $((NUM_DISKS-1))); do
  truncate -s "$SIZE"M "$SCRIPT_DIR/state-$i.img"
  mkfs.ext4 "$SCRIPT_DIR/state-$i.img"
done
