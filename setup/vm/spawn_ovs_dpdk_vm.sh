#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <i> <role> <command>"
    echo "  i: index of the VM"
    echo "  role: server or client"
    echo "  command: test command"
    exit 1
fi

i=$1
ROLE=$2
COMMAND=$3

LOG_DIR="$(dirname "$0")/../../testing/ovs-dpdk/logs"
logfile="$LOG_DIR/vm$i.log"

# Base64 encode the command to avoid space issues in kernel cmdline
TEST_COMMAND_B64=$(echo -n "$COMMAND" | base64 -w 0)

sudo systemd-run --scope --slice=vms.slice \
cloud-hypervisor \
    --cpus boot=1 \
    --memory size=512M,hugepages=on,shared=on,prefault=on \
    --kernel "$TMPDIR/vmlinux.bin" \
    --initramfs /tmp/initramfs-overlay.img \
    --cmdline "console=ttyS0 console=hvc0 rdinit=/init ROLE=$ROLE TEST_COMMAND_B64=$TEST_COMMAND_B64" \
    --disk path="$TMPDIR/vm-img.raw",readonly=on path="$TMPDIR/disks/state-$i.img" path="$TMPDIR/cloudinit/cloudinit-vm$i.img" \
    --net "mac=${NODE_ID}2:34:56:78:92:$(printf '%02X' $i),vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user$i,num_queues=2,vhost_mode=client,queue_size=4096" \
    > "$logfile" 2>&1 &

echo "  VM$i -> $COMMAND"

# mem is NOT bottleneck (tested, same throughput with 256MB)
# queue_size sets both TX/RX's no. of descriptors, larger may reduce warnings of vhost pkt enqueue failures
