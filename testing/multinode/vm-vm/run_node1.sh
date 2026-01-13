#!/usr/bin/env bash
set -e

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 <base_dir> <network> <num_vms> <iteration>" >&2
  exit 1
fi

BASE_DIR=$1
NETWORK=$2
NUM_VMS=$3
ITERATION=$4

log() {
    local ts
    ts=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[$ts] $*" >&2
}

log "Starting run_node1.sh: network=$NETWORK, num_vms=$NUM_VMS, iteration=$ITERATION"

if [ "$NETWORK" != "tap" ] && [ "$NETWORK" != "dpdk" ] && [ "$NETWORK" != "ovs-dpdk" ]; then
  log "Error: network must be tap, dpdk, or ovs-dpdk"
  exit 1
fi

log "Killing existing cloud-hypervisor processes..."
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs -r kill -9" || true
sleep 1

if [ "$NETWORK" = "tap" ]; then
    log "Setting up tap network with $NUM_VMS VMs..."
    ${BASE_DIR}/setup/vm/spawn_vms.sh tap $NUM_VMS vm-server

elif [ "$NETWORK" = "dpdk" ]; then
    log "Setting up DPDK network with $NUM_VMS VMs..."
    sudo rm -rf /mnt/huge/*
    sudo bash -c "ps aux | grep umanet | grep -v grep | awk '{print \$2}' | xargs -r kill -2" || true
    if [ "$ITERATION" -eq 1 ]; then
        log "Building and running umanet (iteration 1)"
        # </dev/null: disconnects stdin so that job runs in background
        nohup sudo ${BASE_DIR}/build_and_run.sh test 32 0 > /dev/null 2>&1 &
        disown
    else
        log "Running umanet (iteration $ITERATION)"
        nohup sudo ${BASE_DIR}/run.sh 32 0 > /dev/null 2>&1 &
        disown
    fi
    sleep 10

    log "Spawning $NUM_VMS VMs with DPDK..."
    ${BASE_DIR}/setup/vm/spawn_vms.sh dpdk $NUM_VMS vm-server

elif [ "$NETWORK" = "ovs-dpdk" ]; then
    log "Setting up OVS-DPDK interfaces with $NUM_VMS VMs..."
    sudo ${BASE_DIR}/setup/ovs/setup_interfaces.sh $NUM_VMS 1
    sleep 10
    log "Spawning $NUM_VMS VMs with OVS-DPDK..."
    ${BASE_DIR}/setup/vm/spawn_vms.sh ovs-dpdk $NUM_VMS vm-server
fi
