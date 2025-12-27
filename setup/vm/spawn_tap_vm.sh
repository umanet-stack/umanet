#!/usr/bin/env bash
set -e


if [ "$#" -ne 5 ]; then
    echo "Usage: $0 <node_id> <i> <res_dir> <role> <command>"
    echo "  node_id: 0 or 1"
    echo "  i: index of the VM"
    echo "  res_dir: directory containing resources"
    echo "  role: server or client"
    echo "  command: iperf3 command"
    exit 1
fi

NODE_ID=$1
i=$2
# e.g. /tmp
RES_DIR=$3
ROLE=$4
COMMAND=$5

LOG_DIR="$(dirname "$0")/../../testing/tap/logs"
logfile="$LOG_DIR/vm$i.log"

# Base64 encode the command to avoid space issues in kernel cmdline
IPERF_COMMAND_B64=$(echo -n "$COMMAND" | base64 -w 0)

sudo systemd-run --scope \
    -p AllowedCPUs=12-27 \
    -p CPUQuota=100% \
cloud-hypervisor \
    --cpus boot=1 \
    --memory size=512M \
    --kernel "$RES_DIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket ROLE=$ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
    --disk path="$RES_DIR/vm-img.raw",readonly=on path="$RES_DIR/disks/state-$i.img" path="$RES_DIR/cloudinit/cloudinit-vm$i.img" \
    --net "tap=tap$i,mac=${NODE_ID}2:34:56:78:90:$(printf '%02X' $i)" \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"
