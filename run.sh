#!/bin/bash

# The executable will be at `build/vhost-switch`.
meson setup build
ninja -C build

sudo ./build/vhost-switch \
  -l 2-3 -n 4 \
  --file-prefix=vhost \
  -- -p 0x1 --socket-file /mnt/huge/sock0 --stats 1