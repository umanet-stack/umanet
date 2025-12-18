#!/bin/bash

PCI_ADDR="$1"
if [ -z "$PCI_ADDR" ]; then
  echo "Usage: $0 <pci-addr>"
  exit 1
fi

# The executable will be at `build/vhost-switch`.
meson setup build -Dc_args="-DDEBUG"
ninja -C build


# EAL (dpdk) options (before --): -l cores, -n memory channels
# Application options (after --): --fp-cores-max, --socket-file path, --stats interval
# 
# Note: For Mellanox NICs, binding is not required (bifurcated driver model).
# However, use -w (whitelist) or -b (blacklist) to avoid DPDK using your SSH NIC:
sudo ./build/vhost-switch \
  -l 2-3 -n 4 \
  --file-prefix=vhost \
  -w $PCI_ADDR \
  -- --fp-cores-max 1 --ip-addr 10.10.1.1/24 --socket-file /mnt/huge/sock0 --socket-file /mnt/huge/sock1 --stats 1