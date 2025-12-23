#!/usr/bin/env bash
set -e


if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <num_vms> <res_dir> <test_mode>"
    echo "  num_vms: number of VMs to spawn"
    echo "  res_dir: directory containing resources"
    echo "  test_mode: test mode (samenode, multinode)"
    exit 1
fi

NUM_VMS=$1
# e.g. /proj/faasnetworkstack-PG0/testing
RES_DIR=$2
VMLINUX_DIR=$RES_DIR
IMG_DIR=$RES_DIR
RW_DISK_DIR=$RES_DIR/disks
TEST_MODE=$3
CLOUDINIT_DIR=/tmp/cloudinit
LOG_DIR="$(dirname "$0")/../../testing/logs"

# Create log directory
rm -rf "$LOG_DIR"/*
mkdir -p "$LOG_DIR"

spawn_vm() {
    local i=$1
    local logfile="$LOG_DIR/vm$i.log"
    local VM_ROLE=""
    local IPERF_COMMAND=""

    if [ "$TEST_MODE" = "samenode" ]; then
        if (( i % 2 == 0 )); then
            VM_ROLE="server"
            IPERF_COMMAND="iperf3 -s"
        else
            VM_ROLE="client"
            IPERF_COMMAND="iperf3 -c 192.168.100.$((i+1)) -P 4 -t 30 -J"
        fi
    elif [ "$TEST_MODE" = "multinode" ]; then
        VM_ROLE="client"
        PORT=$((5200 + i))
        IPERF_COMMAND="iperf3 -c 192.168.100.99 -p $PORT -P 4 -t 30 -J"
    fi
    
    # Base64 encode the command to avoid space issues in kernel cmdline
    IPERF_COMMAND_B64=$(echo -n "$IPERF_COMMAND" | base64 -w 0)

    sudo systemd-run --scope \
        -p AllowedCPUs=4-15 \
        -p CPUQuota=80% \
    cloud-hypervisor \
        --cpus boot=1 \
        --memory size=512M \
        --kernel "$VMLINUX_DIR/vmlinux.bin" \
        --initramfs /tmp/initramfs-overlay.img \
        --cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket ROLE=$VM_ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
        --disk path="$IMG_DIR/vm-img.raw",readonly=on path="$RW_DISK_DIR/state-$i.img" path="$CLOUDINIT_DIR/cloudinit-vm$i.img" \
        --net "tap=tap$i,mac=12:34:56:78:90:$(printf '%02X' $i)" \
        > "$logfile" 2>&1 &
    
    echo "  VM$i -> $logfile"
}

if [ "$TEST_MODE" = "samenode" ]; then
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
fi
if [ "$TEST_MODE" = "multinode" ]; then
    echo "Spawning VMs..."
    for ((i=0; i<NUM_VMS; i++)); do
        spawn_vm "$i"
    done
fi

echo "All VMs launched. Running in background."
