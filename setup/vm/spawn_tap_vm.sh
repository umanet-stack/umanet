#!/usr/bin/env bash
set -e


if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <i> <res_dir> <role> <command>"
    echo "  i: index of the VM"
    echo "  res_dir: directory containing resources"
    echo "  role: server or client"
    echo "  command: iperf3 command"
    exit 1
fi

i=$1
# e.g. /tmp
RES_DIR=$2
ROLE=$3
COMMAND=$4

LOG_DIR="$(dirname "$0")/../../testing/tap/logs"
logfile="$LOG_DIR/vm$i.log"

# Base64 encode the command to avoid space issues in kernel cmdline
IPERF_COMMAND_B64=$(echo -n "$COMMAND" | base64 -w 0)

sudo systemd-run --scope \
    -p AllowedCPUs=4-15 \
    -p CPUQuota=80% \
cloud-hypervisor \
    --cpus boot=1 \
    --memory size=512M \
    --kernel "$RES_DIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket ROLE=$ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
    --disk path="$RES_DIR/vm-img.raw",readonly=on path="$RES_DIR/disks/state-$i.img" path="$RES_DIR/cloudinit/cloudinit-vm$i.img" \
    --net "tap=tap$i,mac=12:34:56:78:90:$(printf '%02X' $i)" \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"
