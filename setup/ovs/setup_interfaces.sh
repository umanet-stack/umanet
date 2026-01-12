#!/bin/bash
set -e

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <num_vms>"
  exit 1
fi

source env.sh
NUM_VMS=$1

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
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-socket-mem=4096
echo "✅ OvS-DPDK configured"

sudo service ovs-dpdk restart
sudo ovs-vsctl get Open_vSwitch . other_config
echo "✅ OVS restarted"
# Enable userspace TSO for better performance
# sudo ovs-vsctl set Open_vSwitch . other_config:userspace-tso-enable=true

# Enable EMC (Exact Match Cache) for fast path packet processing
sudo ovs-vsctl set Open_vSwitch . other_config:emc-enable=true
# Increase max-idle timeout to keep flows in fast path longer (300 seconds)
sudo ovs-vsctl set Open_vSwitch . other_config:max-idle=300000
# Enable PMD auto-load balancing for better distribution
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-auto-lb=true
# Note: vhost-sock-dir doesn't work correctly (treats /tmp/ as relative path)
# With dpdkvhostuser, sockets are created in /usr/local/var/run/openvswitch/

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
sudo ovs-ofctl add-flow ovsbr0 "priority=0,actions=NORMAL"

# NIC port
sudo ovs-vsctl add-port ovsbr0 $NIC \
  -- set Interface $NIC type=dpdk options:dpdk-devargs=$NIC_PCI options:n_rxq=4 options:n_rxq_desc=4096 options:n_txq_desc=4096 
  # options:mtu_request=9000

# ------------------------------------------------------------

echo "[5/6] Create vhost-user ports"
# VMs use num_queues=2 (2 queue pairs = 2 RX + 2 TX queues)
# Configure OVS to match for better performance
for i in $(seq 0 $((NUM_VMS - 1))); do
  sudo ovs-vsctl add-port ovsbr0 vhost-user$i -- \
    set Interface vhost-user$i type=dpdkvhostuser \
    options:n_rxq=1 options:n_txq=1 
    # options:mtu_request=9000
    # other_config:tx-tcp-segmentation=true \
    # other_config:tx-ipv4-checksum=true \
    # other_config:tx-ipv6-checksum=true \
  # important to set for MTU
  # sudo ovs-vsctl set Interface vhost-user$i mtu_request=9000
done

# ------------------------------------------------------------

echo "[6/6] Internal management interface"
sudo ovs-vsctl add-port ovsbr0 ovsbr0-int \
  -- set Interface ovsbr0-int type=internal options:n_rxq=4 options:n_txq=4 
  # options:mtu_request=9000

sudo ip link set ovsbr0-int up
sudo ip addr flush dev ovsbr0-int
sudo ip addr add 192.168.10${NODE_ID}.1/24 dev ovsbr0-int

# Remove any conflicting routes from br0 (TAP bridge) that might interfere
sudo ip route del 192.168.10${NODE_ID}.0/24 dev br0 2>/dev/null || true

# Remove route to other node via br0 and add it via ovsbr0-int instead
if [ "$NODE_ID" = "0" ]; then
  sudo ip route del 192.168.101.0/24 via 192.168.101.1 dev br0 onlink 2>/dev/null || true
  sudo ip route add 192.168.101.0/24 via 192.168.101.1 dev ovsbr0-int onlink || true
elif [ "$NODE_ID" = "1" ]; then
  sudo ip route del 192.168.100.0/24 via 192.168.100.1 dev br0 onlink 2>/dev/null || true
  sudo ip route add 192.168.100.0/24 via 192.168.100.1 dev ovsbr0-int onlink || true
fi

# sudo ip link set ovsbr0-int mtu 9000
# sudo ip link set ovsbr0 mtu 9000
# echo "✅ OVS interfaces mtu set to 9000"

sudo ovs-vsctl show
echo "✅ OVS-DPDK ready."