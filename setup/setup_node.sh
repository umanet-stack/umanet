#!/usr/bin/env bash
set -eu
source env.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Setting up node ${NODE_ID}"

sudo mkdir -p /etc/qemu
sudo bash -c 'echo "allow br0" > /etc/qemu/bridge.conf'
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward
sudo iptables -t nat -A POSTROUTING -s 192.168.10${NODE_ID}.0/24 -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o $(ip route | grep default | awk '{print $5}') -j ACCEPT
sudo iptables -A FORWARD -i $(ip route | grep default | awk '{print $5}') -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT

# delete tap0, br0
for i in $(seq 0 $((MAX_VM_COUNT-1))); do
  sudo ip link delete tap$i 2>/dev/null || true
done
sudo ip link delete br0 2>/dev/null || true
echo "✅ deleted tap0, br0"

# Apply netplan config
sudo cp -f ${SCRIPT_DIR}/netplan-node${NODE_ID}.yaml /etc/netplan/01-netcfg.yaml
sudo netplan apply
echo "✅netplan applied"

# Enable NAT for internet access (optional)
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.10${NODE_ID}.0/24 -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o $(ip route | grep default | awk '{print $5}') -j ACCEPT
sudo iptables -A FORWARD -i $(ip route | grep default | awk '{print $5}') -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT

${SCRIPT_DIR}/clean-disk-state.sh

sudo ip link set $NIC arp on || true
sudo ip link set $NIC multicast on || true

echo "✅ Node ${NODE_ID} setup complete"
