#!/usr/bin/env bash
set -e


if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <num_vms> <res_dir>"
    echo "  num_vms: number of VMs to spawn"
    echo "  res_dir: directory containing resources"
    exit 1
fi

NUM_VMS=$1
# e.g. /proj/faasnetworkstack-PG0/testing
RES_DIR=$2
VMLINUX_DIR=$RES_DIR/kernels
IMG_DIR=$RES_DIR/images
CLOUDINIT_DIR=/tmp/cloudinit
LOG_DIR="$(dirname "$0")/../../testing/logs"

# Create log directory
rm -rf "$LOG_DIR"/*
mkdir -p "$LOG_DIR"

spawn_vm() {
    local i=$1
    local logfile="$LOG_DIR/vm$i.log"

    sudo systemd-run --scope \
        -p AllowedCPUs=7-15 \
        -p CPUQuota=80% \
    cloud-hypervisor \
        --cpus boot=1 \
        --memory size=512M,hugepages=on,shared=true \
        --kernel "$VMLINUX_DIR/vm$i-kernel.bin" \
        --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
        --disk \
            path="$IMG_DIR/vm$i-img.raw" \
            path="$CLOUDINIT_DIR/cloudinit-vm$i.img" \
        --net "mac=12:34:56:78:90:$(printf '%02X' $i),vhost_user=true,socket=/mnt/huge/sock$i,num_queues=2,vhost_mode=client,queue_size=2048" \
        > "$logfile" 2>&1 &
    
    echo "  VM$i -> $logfile"
}

echo "Spawning EVEN VMs (servers)..."
for ((i=0; i<NUM_VMS; i++)); do
    if (( i % 2 == 0 )); then
        spawn_vm "$i"
    fi
done

echo "Waiting 20 seconds for servers to come up..."
sleep 20

echo "Spawning ODD VMs (clients)..."
for ((i=0; i<NUM_VMS; i++)); do
    if (( i % 2 == 1 )); then
        spawn_vm "$i"
    fi
done

echo "All VMs launched. Running in background."
