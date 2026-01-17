#!/bin/bash
set -e

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <num_vms> <queues_per_vm>"
  exit 1
fi

source env.sh
NUM_VMS=$1
QUEUES_PER_VM=$2

if [ "$QUEUES_PER_VM" -lt 1 ] || [ "$QUEUES_PER_VM" -gt 8 ]; then
  echo "Error: queues_per_vm must be between 1 and 8"
  exit 1
fi

# delete all taps, br0
for ((i=0; i<MAX_VM_COUNT; i++)); do
  sudo ip link delete tap$i 2>/dev/null || true
done
sudo ip link delete br0 2>/dev/null || true
echo "✅ br0 and taps deleted"

echo "[1/6] Start OVS-DPDK"
sudo systemctl start ovs-dpdk
sleep 2

echo "[2/6] Enable DPDK in OVS"
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
# cores 0 for slow path
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-lcore-mask=0x01
# cores 1-7 for fast path
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-cpu-mask=0xfe
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-auto-lb=true
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-socket-mem=1024
echo "✅ OVS-DPDK configured"

# disable TSO
# sudo ovs-vsctl set Open_vSwitch . other_config:userspace-tso-enable=False

# Enable EMC (Exact Match Cache) for fast path packet processing
sudo ovs-vsctl set Open_vSwitch . other_config:emc-enable=true
# Increase max-idle timeout to keep flows in fast path longer (300 seconds)
sudo ovs-vsctl set Open_vSwitch . other_config:max-idle=300000
# Enable PMD auto-load balancing for better distribution
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-auto-lb=true

# Note: vhost-sock-dir doesn't work correctly (treats /tmp/ as relative path)
# With dpdkvhostuser, sockets are created in /usr/local/var/run/openvswitch/
sudo service ovs-dpdk restart
sudo ovs-vsctl get Open_vSwitch . other_config
echo "✅ OVS restarted"

ovs-vsctl get Open_vSwitch . dpdk_initialized

# ------------------------------------------------------------

echo "[3/6] Bind NIC to vfio-pci"
sudo modprobe vfio-pci
sudo chmod 666 /dev/vfio/*
sudo ip link set $NIC down || true
sudo dpdk-devbind.py --bind=vfio-pci $NIC_PCI
# sudo dpdk-devbind.py --status

# ------------------------------------------------------------

echo "[4/6] Recreate bridge"
sudo ovs-vsctl --if-exists del-br ovsbr0
sudo ovs-vsctl add-br ovsbr0 -- set bridge ovsbr0 datapath_type=netdev
echo "✅ OVS bridge 'ovsbr0' created"

# Configure bridge for better performance
sudo ovs-vsctl set Bridge ovsbr0 other_config:mac-table-size=10000
sudo ovs-vsctl set Bridge ovsbr0 other_config:disable-in-band=false
# Set fail_mode to standalone for proper L2 learning
sudo ovs-vsctl set Bridge ovsbr0 fail_mode=standalone
# Ensure NORMAL action works for L2 forwarding
sudo ovs-ofctl del-flows ovsbr0 
sudo ovs-ofctl add-flow ovsbr0 "priority=0,actions=FLOOD"

# NIC port
sudo ovs-vsctl add-port ovsbr0 $NIC \
  -- set Interface $NIC type=dpdk options:dpdk-devargs=$NIC_PCI options:n_rxq=8 options:n_rxq_desc=4096 options:n_txq_desc=4096 \
  options:mtu_request=9000 options:max-frame-len=9216

sudo ovs-vsctl set Interface $NIC mtu_request=9000

echo "[5/6] Create vhost-user ports"
for i in $(seq 0 $((NUM_VMS - 1))); do
  sudo ovs-vsctl add-port ovsbr0 vhost-user$i -- \
    set Interface vhost-user$i type=dpdkvhostuser \
    options:n_rxq=$QUEUES_PER_VM options:n_txq=$QUEUES_PER_VM 
    # other_config:tx-tcp-segmentation=true \
    # other_config:tx-ipv4-checksum=true \
    # other_config:tx-ipv6-checksum=true \
  sudo ovs-vsctl set Interface vhost-user$i other_config:tx-tcp-segmentation=true
  sudo ovs-vsctl set Interface vhost-user$i other_config:tx-ipv4-checksum=true
  sudo ovs-vsctl set Interface vhost-user$i other_config:tx-ipv6-checksum=true
  # important to set for MTU
  sudo ovs-vsctl set Interface vhost-user$i mtu_request=9000
done

# Remove any conflicting routes from br0 (TAP bridge) that might interfere
sudo ip route del 192.168.10${NODE_ID}.0/24 dev br0 2>/dev/null || true

# Remove route to other node via br0 and add it via ovsbr0-int instead
if [ "$NODE_ID" = "0" ]; then
  sudo ip route del 192.168.101.0/24 via 192.168.101.1 dev br0 onlink 2>/dev/null || true
elif [ "$NODE_ID" = "1" ]; then
  sudo ip route del 192.168.100.0/24 via 192.168.100.1 dev br0 onlink 2>/dev/null || true
fi

sudo ovs-vsctl show

echo "✅ OVS-DPDK ready."