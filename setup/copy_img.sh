#! /bin/bash

sudo rm -rf /tmp/kernels /tmp/images
mkdir -p /tmp/kernels /tmp/images
for i in {0..31}; do
  sudo cp --reflink=auto /tmp/vmlinux.bin /tmp/kernels/vm$i-kernel.bin &
  sudo cp --reflink=auto /tmp/noble-server-cloudimg-amd64.raw /tmp/images/vm$i-img.raw &
done
wait