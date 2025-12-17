#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Usage: ./setup-node.sh <node_id>
# node_id: 0 or 1

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <node_id> <nic>"
    echo "  node_id: 0 or 1"
    echo "  nic: enp65s0f0np0 or ens1f1np1"
    exit 1
fi

NODE_ID=$1
NIC=$2

if [ "$NODE_ID" != "0" ] && [ "$NODE_ID" != "1" ]; then
    echo "Error: node_id must be 0 or 1"
    exit 1
fi


echo "Setting up node ${NODE_ID}"

sudo rm -f /tmp/vhost-user*

sudo mkdir -p /etc/qemu
sudo bash -c 'echo "allow br0" > /etc/qemu/bridge.conf'
echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward
sudo iptables -t nat -A POSTROUTING -s 192.168.10${NODE_ID}.0/24 -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o $(ip route | grep default | awk '{print $5}') -j ACCEPT
sudo iptables -A FORWARD -i $(ip route | grep default | awk '{print $5}') -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT

# Apply netplan config
sudo cp -f ${SCRIPT_DIR}/netplan-node${NODE_ID}.yaml /etc/netplan/01-netcfg.yaml
sudo netplan apply
echo "✅netplan applied"

sudo ip link set $NIC arp on
sudo ip link set $NIC multicast on

# create tap0, br0
sudo ip link delete tap0 2>/dev/null || true
sudo ip link delete br0 2>/dev/null || true

sudo ip link add name br0 type bridge || true
sudo ip link set br0 up || true
sudo ip addr add 192.168.10${NODE_ID}.1/24 dev br0 || true

sudo ip tuntap add dev tap0 mode tap user $USER || true
sudo ip link set tap0 master br0 || true
sudo ip link set tap0 up || true

# Enable NAT for internet access (optional)
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o $(ip route | grep default | awk '{print $5}') -j ACCEPT
sudo iptables -A FORWARD -i $(ip route | grep default | awk '{print $5}') -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "✅ Node ${NODE_ID} setup complete"
