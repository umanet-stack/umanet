# fahren
## Prerequisites
- use Linux (some syscalls in code are Linux-only)

## Setup
```bash
./setup/init.sh

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
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/tmp/vhost-user1,num_queues=4,vhost_mode=server,queue_size=2048
```