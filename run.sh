#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <num-vms> <show-dash>"
    echo "  num-vms: number of VMs to use"
    echo "  show-dash: 0 or 1"
    exit 1
fi

NUM_VMS="$1"
SHOW_DASH="$2"
# EAL (dpdk) options (before --): -l cores, -n memory channels
# Application options (after --): --fp-cores-max, --socket-file path, --stats interval
# 
# Note: For Mellanox NICs, binding is not required (bifurcated driver model).
# For Intel NICs, device must be bound to vfio-pci (done above).
# Intel ice driver requires DDP package - install it to avoid safe mode limitations
FIRST_CORE=0
LAST_CORE=$((FIRST_CORE + ETH_RX_CORES + ETH_TX_CORES + VHOST_RX_CORES + VHOST_TX_CORES))
echo "✅ Running DPDK on cores $FIRST_CORE-$LAST_CORE, num_vms: $NUM_VMS"

DPDK_DEV_ARG="-a $NIC_PCI"

sudo ./build/umanet \
  -l $FIRST_CORE-$LAST_CORE -n 4 \
  --file-prefix=vhost \
  $DPDK_DEV_ARG \
  --socket-mem 4096,0 \
  --huge-dir /mnt/huge \
  --iova-mode=pa \
  --no-hpet \
  --no-telemetry \
  -- --ip-addr 192.168.10${NODE_ID}.1/24 --socket-dir /mnt/huge --nb-sockets $NUM_VMS --other-node-mac $OTHER_NODE_MAC \
  --eth-rx-cores $ETH_RX_CORES --eth-tx-cores $ETH_TX_CORES \
  --eth-rx-queues $ETH_RX_QUEUES --eth-tx-queues $ETH_TX_QUEUES \
  --vhost-rx-cores $VHOST_RX_CORES --vhost-tx-cores $VHOST_TX_CORES \
  --show-dash $SHOW_DASH --tso $TSO \
  > switch.log 2>&1