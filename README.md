# fahren
## Prerequisites
- use Linux (some syscalls in code are Linux-only)

## What vhost-switch does
**vhost-switch** is a DPDK-based vhost-user backend that acts as a **high-performance network switch** for VMs:

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

# reserve hugepages
# 1024 × 2 MB = 2 GB mem for hugepages
sudo sysctl -w vm.nr_hugepages=1024
grep Huge /proc/meminfo

# mount hugepage FS
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
# /dev/hugepages is default, we use /mnt/huge
mount | grep huge

# load VFIO kernel modules
sudo modprobe vfio
sudo modprobe vfio-pci
lsmod | grep vfio
sudo dmesg | grep -e DMAR -e IOMMU

sudo dpdk-devbind.py --status

# Clean up any leftover hugepage files from previous runs
sudo umount -l /mnt/huge
sudo rm -f /mnt/huge/*
sudo rm -rf /dev/shm/rte_* # remove shm
```

## Building

```bash
meson setup build
ninja -C build
```

The executable will be at `build/vhost-switch`.

## Running
```bash
# Run vhost-switch (vhost-user networking switch)
# EAL options (before --): -l cores, -n memory channels
# Application options (after --): -p portmask, --socket-file path, --stats interval
sudo ./build/vhost-switch \
  -l 2-3 -n 4 \
  --file-prefix=vhost \
  -- -p 0x1 --socket-file /mnt/huge/sock0 --stats 1

# Or install and run from PATH
sudo ninja -C build install
sudo vhost-switch -l 2-3 -n 4 -- -p 0x1 --socket-file /mnt/huge/sock0 --stats 1
```
- **EAL options** (before `--`): `-l` cores, `-n` memory channels, `--huge-dir`, `--file-prefix`, etc.
- **Application options** (after `--`): `-p` portmask, `--socket-file` path, `--stats` interval, etc.

```bash
sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/vmlinux.bin \
  --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
  --disk path=/tmp/noble-server-cloudimg-amd64.raw path=/tmp/cloudinit-vm0-dpdk.img \
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048
```

## Installing Zig
```bash
curl -fL --progress-bar -o /tmp/zig.tar.xz https://ziglang.org/builds/zig-x86_64-linux-0.16.0-dev.1484+d0ba6642b.tar.xz
tar -xf /tmp/zig.tar.xz
mv /tmp/zig-x86_64-linux-0.16.0-dev.1484+d0ba6642b zig
rm /tmp/zig.tar.xz
sudo mv /tmp/zig /usr/bin/zig
echo 'export PATH=$PATH:/usr/bin/zig' >> ~/.bashrc
source ~/.bashrc
```