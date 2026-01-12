#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <build-mode> <num-vms>"
    echo "  build-mode: debug or test"
    echo "  num-vms: number of VMs to use"
    exit 1
fi

BUILD_MODE="$1"
NUM_VMS="$2"

# The executable will be at `build/vhost-switch`.
rm -rf build
if [ "$BUILD_MODE" = "debug" ]; then
  meson setup build -Dc_args="-DDEBUG -DDATAPLANE_STATS" 
else
  meson setup build -Dc_args="-DDATAPLANE_STATS"
fi

ninja -C build

sudo systemctl stop ovs-dpdk
# delete all taps, br0
for ((i=0; i<MAX_VM_COUNT; i++)); do
  sudo ip link delete tap$i 2>/dev/null || true
done
sudo ip link delete br0 2>/dev/null || true
echo "✅ br0 and taps deleted"

# create br0
sudo ip link add name br0 type bridge || true
sudo ip link set br0 up || true
sudo ip addr add 10.10.${NODE_ID+1}.1/24 dev br0 || true
echo "✅ br0 created"

# create taps
for ((i=0; i<NUM_VMS; i++)); do
  sudo ip tuntap add dev tap$i mode tap user $USER || true
  sudo ip link set tap$i master br0 || true
  sudo ip link set tap$i up || true
done
echo "✅ taps created"

source setup/bind-dpdk.sh

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
  > switch.log 2>&1