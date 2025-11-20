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
mount | grep huge

# load VFIO kernel modules
sudo modprobe vfio
sudo modprobe vfio-pci
lsmod | grep vfio
sudo dmesg | grep -e DMAR -e IOMMU

sudo dpdk-devbind.py --status

# Clean up any leftover hugepage files from previous runs
sudo rm -f /dev/hugepages/tas_memory
```

## Running DPDK testpmd
```bash
sudo dpdk-testpmd -l 2-3 -n 4 \
  --pci-whitelist=0000:03:00.1 \
  --vdev 'net_vhost0,iface=/mnt/huge/sock0,queues=1,client=0' \
  --huge-dir=/mnt/huge --file-prefix=vhost -- -i

```