#!/usr/bin/env bash
set -eu
source env.sh


if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <network> <num_vms> <test_mode>"
    echo "  network: network type (tap, dpdk-tap, dpdk, ovs_dpdk)"
    echo "  num_vms: number of VMs to spawn"
    echo "  test_mode: test mode (vm-vm-internal, vm-client, vm-server)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NETWORK=$1
NUM_VMS=$2
TEST_MODE=$3
LOG_DIR="$(dirname "$0")/../../testing/$NETWORK/logs"

# Create log directory
rm -rf "$LOG_DIR"/*
mkdir -p "$LOG_DIR"

spawn_vm() {
    local i=$1
    local VM_ROLE=$2
    local IPERF_COMMAND=$3

    if [ "$NETWORK" = "tap" ]; then
        $SCRIPT_DIR/spawn_tap_vm.sh "$i" "$VM_ROLE" "$IPERF_COMMAND"
    elif [ "$NETWORK" = "dpdk" ] || [ "$NETWORK" = "dpdk-tap" ]; then
        $SCRIPT_DIR/spawn_dpdk_vm.sh "$i" "$VM_ROLE" "$IPERF_COMMAND"
    elif [ "$NETWORK" = "ovs_dpdk" ]; then
        $SCRIPT_DIR/spawn_ovs_dpdk_vm.sh "$i" "$VM_ROLE" "$IPERF_COMMAND"
    else
        echo "Invalid network type: $NETWORK"
        exit 1
    fi
}

if [ "$TEST_MODE" = "vm-vm-internal" ]; then
    echo "Spawning VM-VM-INTERNAL VMs..."
    echo "Spawning EVEN VMs (servers)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 0 )); then
            spawn_vm "$i" "server" "iperf3 -s"
        fi
    done

    echo "Waiting 10 seconds for servers to come up..."
    sleep 10

    echo "Spawning ODD VMs (clients)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 1 )); then
            if [ "$NETWORK" = "tap" ]; then
                spawn_vm "$i" "client" "iperf3 -c 192.168.10${NODE_ID}.$((i+1)) -P 4 -t 30 -J"
            elif [ "$NETWORK" = "dpdk" ]; then
                spawn_vm "$i" "client" "iperf3 -c 192.168.10${NODE_ID}.$((i+1)) -P 4 -t 30 -J"
            elif [ "$NETWORK" = "dpdk-tap" ]; then
                spawn_vm "$i" "client" "iperf3 -c 10.10.${NODE_ID+1}.$((i+1)) -P 4 -t 30 -J"
            elif [ "$NETWORK" = "ovs_dpdk" ]; then
                spawn_vm "$i" "client" "iperf3 -c 10.10.1.$((i+9)) -P 4 -t 30 -J"
            else
                echo "Invalid network type: $NETWORK"
                exit 1
            fi
        fi
    done

elif [ "$TEST_MODE" = "vm-client" ]; then
    echo "Spawning CLIENT VMs... (node 0 only, must run vm-server on node 1 first)"
    for ((i=0; i<NUM_VMS; i++)); do
        spawn_vm "$i" "client" "iperf3 -c 192.168.101.$((i+2)) -P 4 -t 30 -J"
        # if [ "$NETWORK" = "ovs_dpdk" ]; then
        #     PORT=$((PORT + 1))
        #     IPERF_COMMAND="iperf3 -c 10.10.1.1 -p $PORT -P 4 -t 30 -J"
        # fi
    done

elif [ "$TEST_MODE" = "vm-server" ]; then
    echo "Spawning SERVER VMs... (node 1 only)"
    for ((i=0; i<NUM_VMS; i++)); do
        spawn_vm "$i" "server" "iperf3 -s"
    done
fi
# elif [ "$TEST_MODE" = "bm-client" ]; then
#     echo "Spawning BM CLIENTs... (node 0 only, must run vm-server on node 1 first)"
#     for ((i=0; i<NUM_VMS; i++)); do
#         sudo iperf3 -c 192.168.101.$((i+1)) > "$LOG_DIR/bm$i.log" 2>&1 &
#     done
# fi

echo "All VMs launched. Running in background."

echo "Experiment will finish in 60 seconds"
sleep 60
