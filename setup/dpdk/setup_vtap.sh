#!/usr/bin/env bash
set -e

# Usage: ./setup_vtap.sh
# Creates vtap0 <-> veth0 veth pair, with veth0 connected to br0
# Run this BEFORE starting the DPDK app

echo "Setting up veth pair for DPDK <-> br0 communication..."

# delete vtap0, veth0 if they exist
sudo ip link delete vtap0 2>/dev/null || true
sudo ip link delete veth0 2>/dev/null || true
echo "✅ vtap0 and veth0 deleted"

# create vtap0, veth0
sudo ip link add name vtap0 type veth peer name veth0 || true
sudo ip link set veth0 master br0 || true
sudo ip link set vtap0 up || true
sudo ip link set veth0 up || true
echo "✅ vtap0 and veth0 created"
