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

LOG_DIR="$(dirname "$0")/../../testing/dpdk/logs"
logfile="$LOG_DIR/vm$i.log"

# Base64 encode the command to avoid space issues in kernel cmdline
IPERF_COMMAND_B64=$(echo -n "$COMMAND" | base64 -w 0)

# dpdk vms starts from core 5 (tap starts from core 4) since 1 core for dpdk master
# prefault=on when doing zero-copy 
sudo systemd-run --scope \
    -p AllowedCPUs=12-27 \
    -p CPUQuota=100% \
cloud-hypervisor \
    --cpus boot=1 \
    --memory size=512M,hugepages=on,shared=on,prefault=on \
    --kernel "$TMPDIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket ROLE=$ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
    --disk path="$TMPDIR/vm-img.raw",readonly=on path="$TMPDIR/disks/state-$i.img" path="$TMPDIR/cloudinit/cloudinit-vm$i.img" \
    --net tap=tap$i,mac=${NODE_ID}2:34:56:78:91:$(printf '%02X' $i) mac=${NODE_ID}2:34:56:78:90:$(printf '%02X' $i),vhost_user=true,socket=/mnt/huge/sock$i,num_queues=2,vhost_mode=client,queue_size=4096 \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"

# mem is NOT bottleneck (tested, same throughput with 256MB)
# queue_size sets both TX/RX's no. of descriptors, larger may reduce warnings of vhost pkt enqueue failures
