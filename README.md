# fahren
## Prerequisites
- use Linux (some syscalls in code are Linux-only)

1. **VM-to-VM communication**: Forwards packets between VMs based on MAC addresses (software switching)
2. **VM-to-Physical NIC**: Forwards packets from VMs to the physical network interface
3. **Physical NIC-to-VM**: Receives packets from the physical NIC and delivers them to the appropriate VM
4. **MAC learning**: Learns VM MAC addresses from the first packet and maintains a forwarding table
5. **High performance**: Uses DPDK for zero-copy, low-latency packet processing

**Packet flow:**
- **VM → VM**: Packet from VM1's virtio TX queue → vhost-switch → VM2's virtio RX queue
- **VM → Physical**: Packet from VM's virtio TX queue → vhost-switch → Physical NIC TX
- **Physical → VM**: Packet from Physical NIC RX → vhost-switch → VM's virtio RX queue

The switch worker loop continuously:
- Drains packets from physical NIC RX queues and delivers to VMs
- Drains packets from VM virtio TX queues and routes them (to other VMs or physical NIC)

## Setup
- for vm setup, see `setup/setup_vm.md`
```bash
./setup/init-dpdk.sh

# reserve hugepages
# 8192 × 2 MB = 16 GB mem for hugepages
sudo sysctl -w vm.nr_hugepages=8192
grep Huge /proc/meminfo

# mount hugepage FS
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# check mounts
mount | grep huge

# load VFIO kernel modules
sudo modprobe vfio
sudo modprobe vfio-pci
lsmod | grep vfio
sudo dmesg | grep -e DMAR -e IOMMU

# check NICs
sudo dpdk-devbind.py --status

# unmount
sudo umount -l /dev/hugepages

# Clean up any leftover hugepage files from previous runs
sudo umount -l /mnt/huge
sudo rm -f /mnt/huge/*
sudo rm -rf /dev/shm/rte_* # remove shm
sudo rm -f /dev/hugepages/tas_memory
```

## Running
```bash
# c6525-25g nodes
# debug
sudo ./build_and_run.sh 0000:41:00.0 debug 2 32
# test
sudo ./build_and_run.sh 0000:41:00.0 test 2 32

# kill process
sudo ps aux | grep vhost-switch | grep -v grep | awk '{print $2}' | xargs kill -9

# Or install and run from PATH
sudo ninja -C build install
sudo vhost-switch -l 2-3 -n 4 -b 0000:01:00.0 -- --portmask 0x1 --socket-file /mnt/huge/sock0 --stats 1
```

## Development
- `./build_and_run.sh` to check it builds and runs
- spin up a CH VM to test the TCP stack works

### VM packets
eth0/ens4 always send ARP pkt every sec, great for testing vhost connectivity
- **Linux refuses to send ICMP** until ARP resolves.
- pretend VM’s gateway is `02:00:00:00:00:01`

```bash
sudo ip neigh replace 10.10.1.1 lladdr 02:00:00:00:00:01 dev ens4 nud permanent
# or
sudo ip neigh del 10.10.1.1 dev ens4
sudo ip neigh add 10.10.1.1 lladdr 02:00:00:00:00:01 dev ens4 nud permanent

```
- vm will now send TCP/UDP pkts asking for 8.8.8.8
    - pinging pkts will also show
