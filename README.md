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
# build and run
# c6525-25g nodes
sudo ./run.sh 0000:41:00.0

# kill process
sudo ps aux | grep vhost-switch | grep -v grep | awk '{print $2}' | xargs kill -9

# Or install and run from PATH
sudo ninja -C build install
sudo vhost-switch -l 2-3 -n 4 -b 0000:01:00.0 -- --portmask 0x1 --socket-file /mnt/huge/sock0 --stats 1
```

## Development
- `./run.sh` to check it builds and runs
- spin up a CH VM to test the TCP stack works
```bash
sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/vmlinux.bin \
  --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
  --disk path=/tmp/noble-server-cloudimg-amd64.raw path=/tmp/cloudinit-vm0-dpdk.img \
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048
```
