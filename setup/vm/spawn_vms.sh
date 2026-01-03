#!/usr/bin/env bash
set -eu
source env.sh


if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <network> <num_vms> <test_mode> <test>"
    echo "  network: network type (tap, dpdk-tap, dpdk, ovs_dpdk)"
    echo "  num_vms: number of VMs to spawn"
    echo "  test_mode: test mode (vm-vm-internal, vm-client, vm-server)"
    echo "  test: test to run (iperf, sockperf)"
    exit 1
fi

NETWORK=$1
NUM_VMS=$2
TEST_MODE=$3
TEST=$4

VALID_NETWORKS=("tap" "dpdk-tap" "dpdk" "ovs_dpdk")
VALID_TEST_MODES=("vm-vm-internal" "vm-client" "vm-server")
VALID_TESTS=("iperf" "sockperf")

in_array() {
    local value="$1"; shift
    for v in "$@"; do
        [[ "$v" == "$value" ]] && return 0
    done
    return 1
}


if ! in_array "$NETWORK" "${VALID_NETWORKS[@]}"; then
    echo "Error: invalid network '$NETWORK'"
    echo "Valid values: ${VALID_NETWORKS[*]}"
    exit 1
fi

if ! [[ "$NUM_VMS" =~ ^[0-9]+$ ]] || (( NUM_VMS <= 0 )); then
    echo "Error: num_vms must be a positive integer"
    exit 1
fi

if ! in_array "$TEST_MODE" "${VALID_TEST_MODES[@]}"; then
    echo "Error: invalid test_mode '$TEST_MODE'"
    echo "Valid values: ${VALID_TEST_MODES[*]}"
    exit 1
fi

if ! in_array "$TEST" "${VALID_TESTS[@]}"; then
    echo "Error: invalid test '$TEST'"
    echo "Valid values: ${VALID_TESTS[*]}"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$(dirname "$0")/../../testing/$NETWORK/logs"

# Create log directory
rm -rf "$LOG_DIR"/*
mkdir -p "$LOG_DIR"

spawn_vm() {
    local i=$1
    local VM_ROLE=$2
    local TEST_COMMAND=$3

    if [ "$NETWORK" = "tap" ]; then
        $SCRIPT_DIR/spawn_tap_vm.sh "$i" "$VM_ROLE" "$TEST_COMMAND"
    elif [ "$NETWORK" = "dpdk" ] || [ "$NETWORK" = "dpdk-tap" ]; then
        $SCRIPT_DIR/spawn_dpdk_vm.sh "$i" "$VM_ROLE" "$TEST_COMMAND"
    elif [ "$NETWORK" = "ovs_dpdk" ]; then
        $SCRIPT_DIR/spawn_ovs_dpdk_vm.sh "$i" "$VM_ROLE" "$TEST_COMMAND"
    fi
}

if [ "$TEST_MODE" = "vm-vm-internal" ]; then
    echo "Spawning VM-VM-INTERNAL VMs..."
    echo "Spawning EVEN VMs (servers)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 0 )); then
            if [ "$TEST" = "iperf" ]; then
                TEST_COMMAND="iperf3 -s"
            elif [ "$TEST" = "sockperf" ]; then
                TEST_COMMAND="sockperf server -i 192.168.10${NODE_ID}.$((i+2))"
            fi
            spawn_vm "$i" "server" "$TEST_COMMAND"
        fi
    done

    echo "Waiting 10 seconds for servers to come up..."
    sleep 10

    echo "Spawning ODD VMs (clients)..."
    for ((i=0; i<NUM_VMS; i++)); do
        if (( i % 2 == 1 )); then
            if [ "$TEST" = "iperf" ]; then
                if [ "$NETWORK" = "tap" ] || [ "$NETWORK" = "dpdk" ]; then
                    spawn_vm "$i" "iperf-client" "iperf3 -c 192.168.10${NODE_ID}.$((i+1)) -P 4 -t 30 -J"
                elif [ "$NETWORK" = "dpdk-tap" ]; then
                    spawn_vm "$i" "iperf-client" "iperf3 -c 10.10.${NODE_ID+1}.$((i+1)) -P 4 -t 30 -J"
                elif [ "$NETWORK" = "ovs_dpdk" ]; then
                    spawn_vm "$i" "iperf-client" "iperf3 -c 10.10.1.$((i+9)) -P 4 -t 30 -J"
                fi
            elif [ "$TEST" = "sockperf" ]; then
                if [ "$NETWORK" = "tap" ] || [ "$NETWORK" = "dpdk" ]; then
                    spawn_vm "$i" "sockperf-client" "sockperf ping-pong -i 192.168.10${NODE_ID}.$((i+1)) -m 64 -t 30"
                elif [ "$NETWORK" = "dpdk-tap" ]; then
                    spawn_vm "$i" "sockperf-client" "sockperf ping-pong -i 10.10.${NODE_ID+1}.$((i+1)) -m 64 -t 30"
                elif [ "$NETWORK" = "ovs_dpdk" ]; then
                    spawn_vm "$i" "sockperf-client" "sockperf ping-pong -i 10.10.1.$((i+9)) -m 64 -t 30"
                fi
            fi
        fi
    done

elif [ "$TEST_MODE" = "vm-client" ]; then
    echo "Spawning CLIENT VMs... (node 0 only, must run vm-server on node 1 first)"
    for ((i=0; i<NUM_VMS; i++)); do
        if [ "$TEST" = "iperf" ]; then
            spawn_vm "$i" "iperf-client" "iperf3 -c 192.168.101.$((i+2)) -P 4 -t 30 -J"
        elif [ "$TEST" = "sockperf" ]; then
            spawn_vm "$i" "sockperf-client" "sockperf ping-pong -i 192.168.101.$((i+2)) -m 64 -t 30"
        fi
        # if [ "$NETWORK" = "ovs_dpdk" ]; then
        #     PORT=$((PORT + 1))
        #     IPERF_COMMAND="iperf3 -c 10.10.1.1 -p $PORT -P 4 -t 30 -J"
        # fi
    done

elif [ "$TEST_MODE" = "vm-server" ]; then
    echo "Spawning SERVER VMs... (node 1 only)"
    for ((i=0; i<NUM_VMS; i++)); do
        if [ "$TEST" = "iperf" ]; then
            spawn_vm "$i" "iperf-server" "iperf3 -s"
        elif [ "$TEST" = "sockperf" ]; then
            spawn_vm "$i" "sockperf-server" "sockperf server -i 192.168.101.$((i+2))"
        fi
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
