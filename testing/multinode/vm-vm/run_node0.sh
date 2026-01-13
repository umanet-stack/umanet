#!/usr/bin/env bash
set -e

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 <base_dir> <network> <num_vms> <iteration>"
  exit 1
fi

BASE_DIR=$1
NETWORK=$2
NUM_VMS=$3
ITERATION=$4

if [ "$NETWORK" != "tap" ] && [ "$NETWORK" != "dpdk" ] && [ "$NETWORK" != "ovs-dpdk" ]; then
  echo "Error: network must be tap, dpdk, or ovs-dpdk"
  exit 1
fi

sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9" || true

if [ "$NETWORK" = "tap" ]; then
    ${BASE_DIR}/setup/vm/spawn_vms.sh tap $NUM_VMS vm-client

elif [ "$NETWORK" = "dpdk" ]; then
    sudo bash -c "ps aux | grep umanet | grep -v grep | awk '{print \$2}' | xargs kill -9" || true
    if [ "$ITERATION" -eq 1 ]; then
        ${BASE_DIR}/build_and_run.sh test $NUM_VMS
    else
        ${BASE_DIR}/run.sh $NUM_VMS
    fi
    sleep 10
    ${BASE_DIR}/setup/vm/spawn_vms.sh dpdk $NUM_VMS vm-client

elif [ "$NETWORK" = "ovs-dpdk" ]; then
    ${BASE_DIR}/setup/ovs/setup_interfaces.sh $NUM_VMS 1
    sleep 10
    ${BASE_DIR}/setup/vm/spawn_vms.sh ovs-dpdk $NUM_VMS vm-client
fi