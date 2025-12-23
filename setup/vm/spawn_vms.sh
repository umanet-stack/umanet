#!/usr/bin/env bash
set -e


if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <network> <num_vms> <res_dir> <test_mode>"
    echo "  network: network type (tap, dpdk)"
    echo "  num_vms: number of VMs to spawn"
    echo "  res_dir: directory containing resources"
    echo "  test_mode: test mode (samenode, multinode)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NETWORK=$1
NUM_VMS=$2
# e.g. /proj/faasnetworkstack-PG0/testing
RES_DIR=$3
TEST_MODE=$4
LOG_DIR="$(dirname "$0")/../../testing/$NETWORK/logs"

# Create log directory
rm -rf "$LOG_DIR"/*
mkdir -p "$LOG_DIR"

spawn_vm() {
    local i=$1
    local VM_ROLE=""
    local IPERF_COMMAND=""

    if [ "$TEST_MODE" = "samenode" ]; then
        if (( i % 2 == 0 )); then
            VM_ROLE="server"
            IPERF_COMMAND="iperf3 -s"
        else
            VM_ROLE="client"
            IPERF_COMMAND="iperf3 -c 192.168.100.$((i+1)) -P 4 -t 30 -J"
        fi
    elif [ "$TEST_MODE" = "multinode" ]; then
        VM_ROLE="client"
        PORT=$((5200 + i))
        IPERF_COMMAND="iperf3 -c 192.168.100.99 -p $PORT -P 4 -t 30 -J"
    fi
    
    if [ "$NETWORK" = "tap" ]; then
        $SCRIPT_DIR/spawn_tap_vm.sh "$i" "$RES_DIR" "$VM_ROLE" "$IPERF_COMMAND"
    elif [ "$NETWORK" = "dpdk" ]; then
        $SCRIPT_DIR/spawn_dpdk_vm.sh "$i" "$RES_DIR" "$VM_ROLE" "$IPERF_COMMAND"
    else
        echo "Invalid network type: $NETWORK"
        exit 1
    fi
}

if [ "$TEST_MODE" = "samenode" ]; then
    echo "Spawning EVEN VMs (servers)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 0 )); then
            spawn_vm "$i"
        fi
    done

    echo "Waiting 20 seconds for servers to come up..."
    sleep 20

    echo "Spawning ODD VMs (clients)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 1 )); then
            spawn_vm "$i"
        fi
    done
fi
if [ "$TEST_MODE" = "multinode" ]; then
    echo "Spawning VMs..."
    for ((i=0; i<NUM_VMS; i++)); do
        spawn_vm "$i"
    done
fi

echo "All VMs launched. Running in background."
