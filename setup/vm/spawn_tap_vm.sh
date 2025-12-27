#!/usr/bin/env bash
set -eu
source env.sh


if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <i> <role> <command>"
    echo "  i: index of the VM"
    echo "  role: server or client"
    echo "  command: iperf3 command"
    exit 1
fi

i=$1
ROLE=$2
COMMAND=$3

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
    --kernel "$TMPDIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket ROLE=$ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
    --disk path="$TMPDIR/vm-img.raw",readonly=on path="$TMPDIR/disks/state-$i.img" path="$TMPDIR/cloudinit/cloudinit-vm$i.img" \
    --net "tap=tap$i,mac=${NODE_ID}2:34:56:78:90:$(printf '%02X' $i)" \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"
