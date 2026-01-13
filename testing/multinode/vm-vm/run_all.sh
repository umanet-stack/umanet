#!/usr/bin/env bash
set -e
source env.sh

MULTINODE_DIR=${BASE_DIR}/testing/multinode/vm-vm

log() {
    local ts
    ts=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[$ts] $*"
}

if [ "$TEST" = "iperf" ] || [ "$TEST" = "iperf-udp" ]; then
    TEST_CMD="iperf3 $IPERF_CLIENT_OPTIONS"
elif [ "$TEST" = "sockperf" ]; then
    TEST_CMD="sockperf $SOCKPERF_CLIENT_OPTIONS"
fi

ssh -i ~/.ssh/cloudlab "$USER@$NODE1" "cd $BASE_DIR && bash -s" < ${MULTINODE_DIR}/init_node.sh \
  "$BASE_DIR" "$NETWORK" &> "$MULTINODE_DIR/node1.log"
log "⭐️ node 1 initialized"

${MULTINODE_DIR}/init_node.sh "$BASE_DIR" "$NETWORK" &> "$MULTINODE_DIR/node0.log"
log "⭐️ node 0 initialized"

log "======= TEST: $TEST_CMD ======="
for VMS in $(seq 1 1); do
  log "Running test: $VMS VMs (network: $NETWORK)"

  OUTDIR="$BASE_OUT/vms_$VMS"

  ssh -i ~/.ssh/cloudlab "$USER@$NODE1" "cd $BASE_DIR && bash -s" < ${MULTINODE_DIR}/run_node1.sh \
    "$BASE_DIR" "$NETWORK" "$VMS" &> "$MULTINODE_DIR/node1.log"
  log "  started $VMS VMs on node 1"
  sleep 5

  ${MULTINODE_DIR}/run_node0.sh "$BASE_DIR" "$NETWORK" "$VMS" &> "$MULTINODE_DIR/node0.log"
  log "  started $VMS VMs on node 0" 
  sleep 45

  python testing/process_logs/main.py $NETWORK vm-client
done
