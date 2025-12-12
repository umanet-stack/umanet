#!/bin/bash

PCI_ADDR="$1"
if [ -z "$PCI_ADDR" ]; then
  echo "Usage: $0 <pci-addr>"
  exit 1
fi

# The executable will be at `build/vhost-switch`.
meson setup build
ninja -C build

sudo ./build/vhost-switch \
  -l 2-3 -n 4 \
  --file-prefix=vhost \
  -w $PCI_ADDR \
  -- --fp-cores-max 1 --portmask 0x1 --socket-file /mnt/huge/sock0 --stats 1