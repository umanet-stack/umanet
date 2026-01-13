#!/usr/bin/env bash
set -e
source env.sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <num_vms>"
  exit 1
fi

NUM_VMS=$1
MULTINODE_DIR=${BASE_DIR}/testing/multinode/vm-vm
TEST_LOG_DIR=${BASE_DIR}/testing/$NETWORK/$TEST/vm-client
LOG_DIR=$MULTINODE_DIR/log
mkdir -p $LOG_DIR
rm -rf $MULTINODE_DIR/log/*

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
  "$BASE_DIR" "$NETWORK" &>> "$LOG_DIR/node1.log"
log "⭐️ node 1 initialized"

${MULTINODE_DIR}/init_node.sh "$BASE_DIR" "$NETWORK" &>> "$LOG_DIR/node0.log"
log "⭐️ node 0 initialized"

ITERATION=1
log "======= TEST: $TEST_CMD (network: $NETWORK) ======="
for VMS in $(seq 1 $NUM_VMS); do
  log "Running test: $VMS VMs"

  REPORT_DIR="$TEST_LOG_DIR/report-${VMS}vm"
  if [[ -d "$REPORT_DIR" ]] && [[ "$(ls -A "$REPORT_DIR")" ]]; then
      log "  Test result already exists, skipping"
      continue
  fi

  ssh -i ~/.ssh/cloudlab "$USER@$NODE1" "cd $BASE_DIR && bash -s" < ${MULTINODE_DIR}/run_node1.sh \
    "$BASE_DIR" "$NETWORK" "$VMS" "$ITERATION" &>> "$LOG_DIR/node1.log"
  log "  started $VMS VMs on node 1"
  sleep 5

  ${MULTINODE_DIR}/run_node0.sh "$BASE_DIR" "$NETWORK" "$VMS" "$ITERATION" &>> "$LOG_DIR/node0.log"
  log "  started $VMS VMs on node 0" 
  sleep 45

  python testing/process_logs/main.py $NETWORK vm-client &>> "$LOG_DIR/report.log"
  log "  processed logs"

  ITERATION=$((ITERATION + 1))
done
