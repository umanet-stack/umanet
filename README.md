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
  -- -p 0x1 --socket-file /mnt/huge/sock0 --stats 1

# Or install and run from PATH
sudo ninja -C build install
sudo vhost-switch -l 2-3 -n 4 -- -p 0x1 --socket-file /mnt/huge/sock0 --stats 1
```

**Command format:**
- **EAL options** (before `--`): `-l` cores, `-n` memory channels, `--huge-dir`, `--file-prefix`, etc.
- **Application options** (after `--`): `-p` portmask, `--socket-file` path, `--stats` interval, etc.
