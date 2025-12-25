#!/usr/bin/env bash
set -e

# Usage: ./setup_br_tap.sh <node_id>
# node_id: 0 or 1

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <node_id> <nic>"
    echo "  node_id: 0 or 1"
    echo "  nic: enp65s0f0np0 or enp23s0f0np0 or ens1f1np1"
    exit 1
fi

NODE_ID=$1
NIC=$2

if [ "$NODE_ID" != "0" ] && [ "$NODE_ID" != "1" ]; then
    echo "Error: node_id must be 0 or 1"
    exit 1
fi


# delete tap0, br0
for i in {0..31}; do
  sudo ip link delete tap$i 2>/dev/null || true
done
sudo ip link delete br0 2>/dev/null || true
echo "✅ br0 and taps deleted"

# create br0
sudo ip link add name br0 type bridge || true
sudo ip link set br0 up || true
sudo ip addr add 192.168.10${NODE_ID}.1/24 dev br0 || true
echo "✅ br0 created"

if [ "$NIC" = "enp23s0f0np0" ]; then
  sudo dpdk-devbind.py -b ice 0000:17:00.0
  echo "✅ $NIC bound back to ice"
fi

# add nic to br0
sudo ip link set $NIC up
sudo ip addr flush dev $NIC || true
sudo ip link set $NIC master br0 || true
echo "✅ $NIC: removed IP and added to br0"

# create taps
for i in {0..31}; do
  sudo ip tuntap add dev tap$i mode tap user $USER || true
  sudo ip link set tap$i master br0 || true
  sudo ip link set tap$i up || true
done
echo "✅ taps created"