#!/usr/bin/env bash
set -e

VMLINUX_DIR=/proj/faasnetworkstack-PG0/testing/kernels
IMG_DIR=/proj/faasnetworkstack-PG0/testing/images
CLOUDINIT_DIR=/tmp/cloudinit

spawn_vm() {
    local i=$1
    local logfile="testing/logs/vm$i.log"

    sudo cloud-hypervisor \
        --cpus boot=1 \
        --memory size=512M \
        --kernel "$VMLINUX_DIR/vm$i-kernel.bin" \
        --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
        --disk \
            path="$IMG_DIR/vm$i-img.raw" \
            path="$CLOUDINIT_DIR/cloudinit-vm$i.img" \
        --net "tap=tap$i,mac=12:34:56:78:90:$(printf '%02X' $i)" \
        > "$logfile" 2>&1 &
    
    echo "  VM$i -> $logfile"
}

echo "Spawning EVEN VMs (servers)..."
for i in {0..10}; do
    if (( i % 2 == 0 )); then
        spawn_vm "$i"
    fi
done

echo "Waiting 10 seconds for servers to come up..."
sleep 10

echo "Spawning ODD VMs (clients)..."
for i in {0..10}; do
    if (( i % 2 == 1 )); then
        spawn_vm "$i"
    fi
done

echo "All VMs launched. Running in background."
echo "Use 'sudo pkill -9 cloud-hypervisor' to stop all VMs."
