#!/usr/bin/env bash
set -e
source env.sh

MULTINODE_DIR=${BASE_DIR}/testing/multinode/vm-vm

ssh -i ~/.ssh/cloudlab "$USER@$NODE1" "cd $BASE_DIR && bash -s" < ${MULTINODE_DIR}/init_node.sh \
  "$BASE_DIR" "$NETWORK" 2>&1 | tee "$MULTINODE_DIR/node1.log"
echo "⭐️ node 1 initialized"

${MULTINODE_DIR}/init_node.sh "$BASE_DIR" "$NETWORK" 2>&1 | tee "$MULTINODE_DIR/node0.log"
echo "⭐️ node 0 initialized"

for VMS in $(seq 1 1); do
  echo "Running test: $VMS VMs (network: $NETWORK)"

  OUTDIR="$BASE_OUT/vms_$VMS"

  ssh -i ~/.ssh/cloudlab "$USER@$NODE1" "cd $BASE_DIR && bash -s" < ${MULTINODE_DIR}/run_node1.sh \
    "$BASE_DIR" "$NETWORK" "$VMS" 2>&1 | tee "$MULTINODE_DIR/node1.log"
  echo "  started $VMS VMs on node 1"
  sleep 5

  ${MULTINODE_DIR}/run_node0.sh "$BASE_DIR" "$NETWORK" "$VMS" 2>&1 | tee "$MULTINODE_DIR/node0.log"
  sleep 45

  python testing/process_logs/main.py $NETWORK vm-client
done
