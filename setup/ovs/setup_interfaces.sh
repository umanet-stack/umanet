#!/bin/bash
set -e

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <num_vms>"
  exit 1
fi

source env.sh
NUM_VMS=$1

echo "[1/6] Start OVS-DPDK"
sudo systemctl start ovs-dpdk
sleep 2

echo "[2/6] Enable DPDK in OVS"
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-lcore-mask=0x0f
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-cpu-mask=0xf0
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-socket-mem=1024

ovs-vsctl get Open_vSwitch . dpdk_initialized

# ------------------------------------------------------------

echo "[3/6] Bind NIC to vfio-pci"
sudo modprobe vfio-pci
sudo chmod 666 /dev/vfio/*
sudo ip link set $NIC down || true
sudo dpdk-devbind.py --bind=vfio-pci $NIC_PCI
sudo dpdk-devbind.py --status

# ------------------------------------------------------------

echo "[4/6] Recreate bridge"
sudo ovs-vsctl --if-exists del-br ovsbr0
sudo ovs-vsctl add-br ovsbr0 -- set bridge ovsbr0 datapath_type=netdev

# NIC port
sudo ovs-vsctl add-port ovsbr0 $NIC \
  -- set Interface $NIC type=dpdk options:dpdk-devargs=$NIC_PCI options:n_rxq=4

# ------------------------------------------------------------

echo "[5/6] Create vhost-user ports"
for i in $(seq 0 $((NUM_VMS - 1))); do
  sudo ovs-vsctl add-port ovsbr0 vhost-user$i -- \
    set Interface vhost-user$i type=dpdkvhostuserclient \
    options:vhost-server-path=/tmp/vhost-user$i \
    options:n_rxq=4
done

# ------------------------------------------------------------

echo "[6/6] Internal management interface"
sudo ovs-vsctl add-port ovsbr0 ovsbr0-int \
  -- set Interface ovsbr0-int type=internal options:n_rxq=1

sudo ip link set ovsbr0-int up
sudo ip addr flush dev ovsbr0-int
sudo ip addr add 192.168.10${NODE_ID}.1/24 dev ovsbr0-int

echo "OVS-DPDK ready."
