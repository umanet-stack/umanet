#!/usr/bin/env bash
set -ex

SCRIPT_DIR="/tmp/disks"

rm -rf "$SCRIPT_DIR"
mkdir -p "$SCRIPT_DIR"

for i in {0..31}; do
  truncate -s 64M "$SCRIPT_DIR/state-$i.img"
  mkfs.ext4 "$SCRIPT_DIR/state-$i.img"
done
