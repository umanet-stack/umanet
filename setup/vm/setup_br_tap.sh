#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <num_vms>"
    echo "  num_vms: number of VMs"
    exit 1
fi

NUM_VMS=$1

# delete tap0, br0
for ((i=0; i<NUM_VMS; i++)); do
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

# add route to other node
# When a physical interface is added to a bridge, the interface becomes a bridge port (no IP).
# The bridge (br0) gets the IP address. Routes should reference the bridge, not the physical interface
if [ "$NODE_ID" = "0" ]; then
  sudo ip route add 192.168.101.0/24 via 192.168.101.1 dev br0 onlink || true
elif [ "$NODE_ID" = "1" ]; then
  sudo ip route add 192.168.100.0/24 via 192.168.100.1 dev br0 onlink || true
fi
echo "✅ route added to other node"

# create taps
for ((i=0; i<NUM_VMS; i++)); do
  sudo ip tuntap add dev tap$i mode tap user $USER || true
  sudo ip link set tap$i master br0 || true
  sudo ip link set tap$i up || true
  # disable TSO, GSO, GRO, scatter gather offloads
  # sudo ethtool -K tap$i tso off gso off gro off sg off || true
  # sudo ethtool -K tap$i gro off || true
done
echo "✅ taps created"