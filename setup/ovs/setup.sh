if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <num_vms>"
    echo "  num_vms: number of VMs"
    exit 1
fi

NUM_VMS=$1

modprobe openvswitch

sudo ovs-vsctl init
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-lcore-mask=0xf
sudo ovs-vsctl set Open_vSwitch . other_config:pmd-cpu-mask=0xf0
sudo ovs-vsctl set Open_vSwitch . other_config:dpdk-socket-mem=1024
sudo ovs-vsctl list Open_vSwitch

sudo ovs-vsctl add-br ovsbr0 -- set bridge ovsbr0 datapath_type=netdev
for i in $(seq 0 $NUM_VMS); do
    sudo ovs-vsctl add-port ovsbr0 vhost-user$i -- set Interface vhost-user$i type=dpdkvhostuserclient options:vhost-server-path=/tmp/vhost-user$i -- set Interface vhost-user$i options:n_rxq=4
done
sudo ovs-vsctl add-port ovsbr0 enp65s0f0np0 -- set Interface enp65s0f0np0 type=dpdk options:dpdk-devargs=0000:41:00.0 options:n_rxq=4

sudo ovs-vsctl add-port ovsbr0 ovsbr0-int -- set Interface ovsbr0-int type=internal
sudo ovs-vsctl set Interface ovsbr0-int options:n_rxq=4
sudo ip link set ovsbr0-int up
sudo ip addr add 10.10.1.2/24 dev ovsbr0-int
