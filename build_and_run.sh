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
  meson setup build -Dc_args="-DDEBUG"
else
  meson setup build
fi

ninja -C build

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
  -w $PCI_ADDR \
  -- --fp-cores-max $FP_CORES_MAX --ip-addr 192.168.100.1/24 --socket-dir /mnt/huge --nb-sockets $NUM_VMS --stats 1