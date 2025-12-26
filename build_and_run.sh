#!/bin/bash

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 <nic> <pci-addr> <build-mode> <fp-cores-max> <num-vms>"
    echo "  nic: enp65s0f0np0 or enp23s0f0np0 or ens1f1np1"
    echo "  pci-addr: PCI address of the NIC"
    echo "  build-mode: debug or test"
    echo "  fp-cores-max: number of cores to use for the fast path"
    echo "  num-vms: number of VMs to use"
    exit 1
fi

NIC="$1"
PCI_ADDR="$2"
BUILD_MODE="$3"
FP_CORES_MAX="$4"
NUM_VMS="$5"

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

# Check if device is bound to vfio-pci (Intel NICs need this)
DEVICE_INFO=$(dpdk-devbind.py --status 2>/dev/null | grep "$PCI_ADDR" || echo "")
CURRENT_DRIVER=$(echo "$DEVICE_INFO" | grep -o "drv=[^ ]*" | cut -d= -f2 || echo "")
DEVICE_NAME=$(echo "$DEVICE_INFO" | grep -o "'[^']*'" | head -1 | tr -d "'" || echo "")

USE_VFIO=false
IS_INTEL=false

# Check if it's an Intel device by name or driver
if echo "$DEVICE_NAME" | grep -qi "E810\|Ethernet Controller.*1592\|Ethernet Controller.*159b"; then
  IS_INTEL=true
  
  # Check for DDP package (required for Intel ice driver, not safe mode)
  DDP_PKG="/lib/firmware/intel/ice/ddp/ice.pkg"
  if [ ! -f "$DDP_PKG" ]; then
    # Try to decompress if .zst exists
    DDP_ZST="/lib/firmware/intel/ice/ddp/ice.pkg.zst"
    if [ -f "$DDP_ZST" ] && command -v zstd >/dev/null 2>&1; then
      echo "🔧 Decompressing DDP package..."
      sudo zstd -d "$DDP_ZST" -o "$DDP_PKG" 2>/dev/null || true
    fi
    
    if [ ! -f "$DDP_PKG" ]; then
      echo "⚠️  WARNING: DDP package not found at $DDP_PKG"
      echo "⚠️  Intel ice driver requires DDP package for full functionality"
      echo "⚠️  Install it from: https://www.intel.com/content/www/us/en/download/19779/"
      echo "⚠️  Or decompress: sudo zstd -d /lib/firmware/intel/ice/ddp/ice.pkg.zst -o $DDP_PKG"
    else
      echo "✅ DDP package found at $DDP_PKG"
    fi
  else
    echo "✅ DDP package found at $DDP_PKG"
  fi
fi

if [ "$CURRENT_DRIVER" = "vfio-pci" ]; then
  USE_VFIO=true
  echo "ℹ️  Device already bound to vfio-pci, skipping kernel interface configuration"
elif [ "$CURRENT_DRIVER" = "ice" ] || [ "$CURRENT_DRIVER" = "i40e" ] || [ "$CURRENT_DRIVER" = "ixgbe" ]; then
  IS_INTEL=true
  echo "🔧 Intel NIC detected, binding to vfio-pci for DPDK..."
  sudo modprobe vfio-pci || true
  sudo ip link set $NIC down 2>/dev/null || true
  if sudo dpdk-devbind.py -b vfio-pci $PCI_ADDR 2>/dev/null; then
    USE_VFIO=true
    echo "✅ NIC bound to vfio-pci (kernel networking disabled for this NIC)"
  else
    echo "⚠️  Warning: Failed to bind to vfio-pci, will try -a flag"
  fi
fi

# Only configure kernel interface if not using vfio-pci
if [ "$USE_VFIO" = "false" ]; then
  sudo ip addr flush dev $NIC 2>/dev/null || true
  sudo ip link set $NIC nomaster 2>/dev/null || true
  sudo ip addr add 192.168.100.1/24 dev $NIC 2>/dev/null || true
  sudo ip link set $NIC up 2>/dev/null || true
  echo "✅ set $NIC IP to 192.168.100.1/24 and removed from br0"
fi

# EAL (dpdk) options (before --): -l cores, -n memory channels
# Application options (after --): --fp-cores-max, --socket-file path, --stats interval
# 
# Note: For Mellanox NICs, binding is not required (bifurcated driver model).
# For Intel NICs, device must be bound to vfio-pci (done above).
# Intel ice driver requires DDP package - install it to avoid safe mode limitations
FIRST_CORE=0
LAST_CORE=$((FIRST_CORE + FP_CORES_MAX))
echo "✅ Running DPDK on cores $FIRST_CORE-$LAST_CORE, num_vms: $NUM_VMS"

DPDK_DEV_ARG="-a $PCI_ADDR"

sudo ./build/vhost-switch \
  -l $FIRST_CORE-$LAST_CORE -n 4 \
  --file-prefix=vhost \
  $DPDK_DEV_ARG \
  --socket-mem 4096,0 \
  --huge-dir /mnt/huge \
  --iova-mode=pa \
  --no-hpet \
  --no-telemetry \
  -- --fp-cores-max $FP_CORES_MAX --ip-addr 192.168.100.1/24 --socket-dir /mnt/huge --nb-sockets $NUM_VMS