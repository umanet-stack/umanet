#!/bin/bash

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <pci-addr> <build-mode> <fp-cores-max> <num-vms>"
    echo "  pci-addr: PCI address of the NIC"
    echo "  build-mode: debug or test"
    echo "  fp-cores-max: number of cores to use for the fast path"
    echo "  num-vms: number of VMs to use"
    exit 1
fi

PCI_ADDR="$1"
BUILD_MODE="$2"
FP_CORES_MAX="$3"
NUM_VMS="$4"

# The executable will be at `build/vhost-switch`.
rm -rf build
if [ "$BUILD_MODE" = "debug" ]; then
  meson setup build -Dc_args="-DDEBUG -DDATAPLANE_STATS" 
else
  meson setup build -Dc_args="-DDATAPLANE_STATS"
fi

ninja -C build

# delete tap0, br0
for i in {0..31}; do
  sudo ip link delete tap$i 2>/dev/null || true
done
sudo ip link delete br0 2>/dev/null || true
echo "✅ br0 and taps deleted"

sudo ip addr flush dev enp65s0f0np0 || true
sudo ip link set enp65s0f0np0 nomaster || true
sudo ip addr add 192.168.100.1/24 dev enp65s0f0np0 || true
sudo ip link set enp65s0f0np0 up || true
echo "✅ set enp65s0f0np0 IP to 192.168.100.1/24 and removed from br0"

# EAL (dpdk) options (before --): -l cores, -n memory channels
# Application options (after --): --fp-cores-max, --socket-file path, --stats interval
# 
# Note: For Mellanox NICs, binding is not required (bifurcated driver model).
# However, use -w (whitelist) or -b (blacklist) to avoid DPDK using your SSH NIC:
FIRST_CORE=0
LAST_CORE=$((FIRST_CORE + FP_CORES_MAX))
echo "✅ Running DPDK on cores $FIRST_CORE-$LAST_CORE, num_vms: $NUM_VMS"
sudo ./build/vhost-switch \
  -l $FIRST_CORE-$LAST_CORE -n 4 \
  --file-prefix=vhost \
  -a $PCI_ADDR \
  --socket-mem 4096,0 \
  --huge-dir /mnt/huge \
  --iova-mode=pa \
  --no-hpet \
  --no-telemetry \
  -- --fp-cores-max $FP_CORES_MAX --ip-addr 192.168.100.1/24 --socket-dir /mnt/huge --nb-sockets $NUM_VMS --stats 0