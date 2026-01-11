#!/bin/bash

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <num_vms>"
    echo "  num_vms: number of VMs"
    exit 1
fi

source env.sh

NUM_VMS=$1

sudo modprobe vfio-pci
sudo /usr/bin/chmod a+x /dev/vfio
sudo /usr/bin/chmod 0666 /dev/vfio/*
sudo ip link set $NIC down || true
sudo dpdk-devbind.py --bind=vfio-pci $NIC || true
sudo dpdk-devbind.py --status

modprobe openvswitch

# Start OVS service if not running
if ! systemctl is-active --quiet openvswitch; then
    sudo systemctl start openvswitch
    sleep 2  # Give OVS time to start
fi

sudo ovs-vsctl init
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-lcore-mask=0xf
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-cpu-mask=0xf0
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-socket-mem=1024
sudo ovs-vsctl list Open_vSwitch

# Remove existing bridge if it exists (this also removes all ports)
if sudo ovs-vsctl br-exists ovsbr0; then
    sudo ovs-vsctl del-br ovsbr0
fi

sudo ovs-vsctl add-br ovsbr0 -- set bridge ovsbr0 datapath_type=netdev
for i in $(seq 0 $NUM_VMS); do
    sudo ovs-vsctl add-port ovsbr0 vhost-user$i -- \
    set Interface vhost-user$i type=dpdkvhostuserserver options:vhost-server-path=/tmp/vhost-user$i -- \
    set Interface vhost-user$i options:n_rxq=4
done
sudo ovs-vsctl add-port ovsbr0 $NIC -- set Interface $NIC type=dpdk options:dpdk-devargs=$NIC_PCI options:n_rxq=4

sudo ovs-vsctl add-port ovsbr0 ovsbr0-int -- set Interface ovsbr0-int type=internal
sudo ovs-vsctl set Interface ovsbr0-int options:n_rxq=4
sudo ip link set ovsbr0-int up
# Remove existing IP if present, then add it
sudo ip addr del 10.10.1.2/24 dev ovsbr0-int 2>/dev/null || true
sudo ip addr add 10.10.1.2/24 dev ovsbr0-int
