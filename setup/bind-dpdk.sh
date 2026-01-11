#!/usr/bin/env bash
set -eu
source env.sh


# Check if device is bound to vfio-pci (Intel NICs need this)
DEVICE_INFO=$(dpdk-devbind.py --status 2>/dev/null | grep "$NIC_PCI" || echo "")
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
  if sudo dpdk-devbind.py -b vfio-pci $NIC_PCI 2>/dev/null; then
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
  sudo ip addr add 192.168.10${NODE_ID}.1/24 dev $NIC 2>/dev/null || true
  sudo ip link set $NIC up 2>/dev/null || true
  echo "✅ set $NIC IP to 192.168.10${NODE_ID}.1/24 and removed from br0"
fi