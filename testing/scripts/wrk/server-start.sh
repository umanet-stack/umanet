#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <i>"
  echo "  i: index of the VM"
  exit 1
fi

i=$1

CPU="1"
MEM="512M"
QUEUES="2"

# Common args
ch_args=(
  --cpus "boot=$CPU"
  --kernel "$TMPDIR/vmlinux.bin"
  --initramfs /tmp/initramfs-overlay.img
  --cmdline "console=ttyS0 console=hvc0 rdinit=/init"
  --disk "path=/tmp/modified.qcow2,readonly=on" "path=/tmp/disks/state-$i.img" "path=/tmp/cloudinit/cloudinit-vm$i.img"
)

# Memory differs slightly
if [ "$NETWORK" = "ovs-dpdk" ]; then
  ch_args+=( --memory "size=$MEM,hugepages=on,shared=on,prefault=on" )
else
  ch_args+=( --memory "size=$MEM" )
fi

# Network-specific
case "$NETWORK" in
  ovs-dpdk)
    ch_args+=(
      --net "mac=${NODE_ID}2:34:56:78:92:$(printf '%02X' "$i"),vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user$i,num_queues=$QUEUES,vhost_mode=client,queue_size=4096"
    )
    ;;
  tap)
    ch_args+=(
      --net "tap=tap$i,mac=${NODE_ID}2:34:56:78:90:$(printf '%02X' "$i")"
    )
    ;;
  *)
    echo "Invalid network: $NETWORK"
    exit 1
    ;;
esac

sudo systemd-run --scope --slice=vms.slice \
  cloud-hypervisor "${ch_args[@]}"
