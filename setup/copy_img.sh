#! /bin/bash

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <num_vms>"
    echo "  num_vms: number of VMs to copy"
    exit 1
fi

NUM_VMS=$1
DEST_DIR=/proj/faasnetworkstack-PG0/testing

sudo rm -rf $DEST_DIR/kernels/* $DEST_DIR/images/*
sudo mkdir -p $DEST_DIR/kernels $DEST_DIR/images
for ((i=0; i<NUM_VMS; i++)); do
  (
    sudo cp /tmp/vmlinux.bin $DEST_DIR/kernels/vm$i-kernel.bin
    sudo cp /tmp/noble-server-cloudimg-amd64.raw $DEST_DIR/images/vm$i-img.raw
    echo "✅ Copied vm$i-kernel.bin and vm$i-img.raw"
  ) &
done
wait