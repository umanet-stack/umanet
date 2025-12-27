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

LOG_DIR="$(dirname "$0")/../../testing/ovs_dpdk/logs"
logfile="$LOG_DIR/vm$i.log"

# Base64 encode the command to avoid space issues in kernel cmdline
IPERF_COMMAND_B64=$(echo -n "$COMMAND" | base64 -w 0)

# dpdk vms starts from core 5 (tap starts from core 4) since 1 core for dpdk master
# prefault=on when doing zero-copy
sudo systemd-run --scope \
    -p AllowedCPUs=9-24 \
    -p CPUQuota=80% \
cloud-hypervisor \
    --cpus boot=1 \
    --memory size=512M,hugepages=on,shared=on,prefault=on \
    --kernel "$RES_DIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init VM_INDEX=$i ROLE=$ROLE IPERF_COMMAND_B64=$IPERF_COMMAND_B64" \
    --disk path="$RES_DIR/noble-server-cloudimg-amd64-customized.raw",readonly=on path="$RES_DIR/disks/state-$i.img" \
    --net "mac=12:34:56:78:90:$(printf '%02X' $i),vhost_user=true,socket=/mnt/huge/sock$i,num_queues=2,vhost_mode=server,socket=/tmp/vhost-user$i,queue_size=4096" \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"

# mem is NOT bottleneck (tested, same throughput with 256MB)
# queue_size sets both TX/RX's no. of descriptors, larger may reduce warnings of vhost pkt enqueue failures
