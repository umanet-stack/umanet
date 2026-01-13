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

run_with_retry() {
    local cmd="$1"
    local max_retries="${2:-10}"
    local check_delay="${3:-3}"
    local attempt=1
    local script_pid
    local umanet_pid
    
    while [ $attempt -le $max_retries ]; do
        log "Attempt $attempt/$max_retries: Running command in background..."
        
        # Run command in background and capture script PID
        log "Changing to directory: $BASE_DIR"
        cd "$BASE_DIR" || {
            log "ERROR: Failed to cd to $BASE_DIR"
            exit 1
        }
        log "Current directory: $(pwd)"
        log "About to execute: $cmd"
        # Run command in background with stdin redirected to prevent blocking
        # The command output goes to switch.log (from build_and_run.sh), so we redirect here too
        bash -c "$cmd" < /dev/null > /dev/null 2>&1 &
        script_pid=$!
        log "Script started with PID $script_pid, waiting ${check_delay}s for umanet to start..."
        
        # Wait for umanet process to appear
        sleep $check_delay
        
        umanet_pid=$(sudo bash -c "ps aux | grep '[u]manet' | grep -v 'grep' | awk '{print \$2}' | head -1" || echo "")
        # umanet_pid=$(pgrep -u $(whoami) umanet | head -1 || echo "")
        
        if [ -n "$umanet_pid" ]; then
            sleep 2
            if sudo kill -0 "$umanet_pid" 2>/dev/null; then
                log "✅ umanet process (PID $umanet_pid) is running and stable, continuing..."
                # Detach the script so it can continue running
                disown $script_pid 2>/dev/null || true
                return 0
            else
                log "❌ umanet process (PID $umanet_pid) died shortly after starting (segfault)"
            fi
        else
            log "❌ umanet process did not start or died immediately (likely segfault)"
        fi
        
        if kill -0 $script_pid 2>/dev/null; then
            kill $script_pid 2>/dev/null || true
            wait $script_pid 2>/dev/null || true
        fi
        
        attempt=$((attempt + 1))
        if [ $attempt -le $max_retries ]; then
            log "Retrying in 2 seconds..."
            sleep 2
        fi
    done
    
    log "ERROR: umanet failed to start and stay running after $max_retries attempts"
    return 1
}

log "Starting run_node1.sh: network=$NETWORK, num_vms=$NUM_VMS, iteration=$ITERATION"

if [ "$NETWORK" != "tap" ] && [ "$NETWORK" != "dpdk" ] && [ "$NETWORK" != "ovs-dpdk" ]; then
  log "Error: network must be tap, dpdk, or ovs-dpdk"
  exit 1
fi

log "Killing existing cloud-hypervisor processes..."
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs -r kill -9" || true

if [ "$NETWORK" = "tap" ]; then
    log "Setting up tap network with $NUM_VMS VMs..."
    ${BASE_DIR}/setup/vm/spawn_vms.sh tap $NUM_VMS vm-server

elif [ "$NETWORK" = "dpdk" ]; then
    log "Setting up DPDK network with $NUM_VMS VMs..."
    if [ "$ITERATION" -eq 1 ]; then
        log "Building and running umanet (iteration 1) with retry logic..."
        run_with_retry "sudo ${BASE_DIR}/build_and_run.sh test $NUM_VMS 0" 3 5
    else
        log "Running umanet (iteration $ITERATION) with retry logic..."
        run_with_retry "sudo ${BASE_DIR}/run.sh $NUM_VMS 0" 3 5
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

log "Completed run_node1.sh"