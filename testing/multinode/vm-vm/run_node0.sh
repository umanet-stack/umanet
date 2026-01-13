#!/usr/bin/env bash
set -e

if [ "$#" -ne 3 ]; then
  echo "Usage: $0 <base_dir> <network> <num_vms>"
  exit 1
fi

BASE_DIR=$1
NETWORK=$2
NUM_VMS=$3

if [ "$NETWORK" != "tap" ] && [ "$NETWORK" != "dpdk" ] && [ "$NETWORK" != "ovs-dpdk" ]; then
  echo "Error: network must be tap, dpdk, or ovs-dpdk"
  exit 1
fi

sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"

if [ "$NETWORK" = "tap" ]; then
    ${BASE_DIR}/setup/vm/spawn_vms.sh tap $NUM_VMS vm-client
fi