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
```bash
./setup/init.sh
./setup/create-cloud-init.sh
./setup/init-dpdk.sh
# reserve and mount hugepages

# reserve hugepages
# 2048 × 2 MB = 4 GB mem for hugepages
sudo sysctl -w vm.nr_hugepages=2048
grep Huge /proc/meminfo

# mount hugepage FS
sudo mount -t hugetlbfs nodev /dev/hugepages

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
sudo ./build_and_run.sh 0000:41:00.0 debug
# test
sudo ./build_and_run.sh 0000:41:00.0

# kill process
sudo ps aux | grep vhost-switch | grep -v grep | awk '{print $2}' | xargs kill -9

# Or install and run from PATH
sudo ninja -C build install
sudo vhost-switch -l 2-3 -n 4 -b 0000:01:00.0 -- --portmask 0x1 --socket-file /mnt/huge/sock0 --stats 1
```

## Setup VM img/fs
```bash
# c6525-25g
./setup/vanilla/setup_node.sh 0 enp65s0f0np0

# xl170
./setup/vanilla/setup_node.sh 0 ens1f1np1

sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/tmp/noble-server-cloudimg-amd64.raw path=/tmp/cloudinit-vm0.img \
	--net "tap=tap0,mac=52:54:00:02:d9:01" 

sudo apt update
sudo apt install -y iperf sockperf
sudo rm /etc/netplan/*.yaml
sudo systemctl disable systemd-networkd-wait-online.service

sudo rm -f /tmp/vm*-img.raw /tmp/vm*-kernel.bin
cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm0-img.raw
cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm1-img.raw
cp /tmp/vmlinux.bin /tmp/vm0-kernel.bin
cp /tmp/vmlinux.bin /tmp/vm1-kernel.bin
```

## Development
- `./build_and_run.sh` to check it builds and runs
- spin up a CH VM to test the TCP stack works
```bash
sudo ip addr del 10.10.1.10/24 dev ens4

sudo ip link set ens4 up
sudo ip addr add 10.10.1.10/24 dev ens4
sudo ip route add default via 10.10.1.1

sudo ip link set ens4 up
sudo ip addr add 10.10.1.20/24 dev ens4
sudo ip route add default via 10.10.1.1

./setup/vanilla/setup_node.sh 0 enp65s0f0np0
# vm0
sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/vm0-kernel.bin \
  --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
  --disk path=/tmp/vm0-img.raw path=/tmp/cloudinit-vm0-dpdk.img \
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048

./setup/vanilla/setup_node.sh 0 enp65s0f0np0
# vm1 - NOTE: Uses sock1 (different from vm0)
sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/vm1-kernel.bin \
  --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
  --disk path=/tmp/vm1-img.raw path=/tmp/cloudinit-vm1-dpdk.img \
  --net mac=52:54:20:11:C5:02,vhost_user=true,socket=/mnt/huge/sock1,num_queues=2,vhost_mode=client,queue_size=2048

ps aux | grep cloud-hypervisor | grep -v grep | awk '{print $2}' | xargs kill -9
```

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
    
### Testing
```bash
# no. of TX/RX queues in NIC e.g. combined 32 = 32TX + 32RX
# canonical: 1 core uses 1TX + 1RX
ethtool -l enp65s0f0np0

iperf -s
iperf -c 10.10.1.10
```