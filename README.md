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

## Running DPDK testpmd
```bash
dpdk-testpmd -v # 19.11.14

sudo dpdk-testpmd -l 2-3 -n 4 \
  --pci-whitelist=0000:03:00.1 \
  --vdev 'net_vhost0,iface=/mnt/huge/sock0,queues=1,client=0' \
  --huge-dir=/mnt/huge --file-prefix=vhost -- -i

```