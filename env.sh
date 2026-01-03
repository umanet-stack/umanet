#!/usr/bin/env bash
set -a          # automatically export all variables
source .env     # load key=value pairs
set +a

# Validation (fail fast)
: "${NODE_ID:?missing NODE_ID}"
: "${NIC:?missing NIC}"
: "${NIC_PCI:?missing NIC_PCI}"
: "${TMPDIR:?missing TMPDIR}"
: "${TEST:?missing TEST}"
: "${ETH_RX_CORES:?missing ETH_RX_CORES}"
: "${ETH_TX_CORES:?missing ETH_TX_CORES}"
: "${VHOST_RX_CORES:?missing VHOST_RX_CORES}"
: "${VHOST_TX_CORES:?missing VHOST_TX_CORES}"

MAX_VM_COUNT=64

if [ "$NODE_ID" != "0" ] && [ "$NODE_ID" != "1" ]; then
    echo "Error: node_id must be 0 or 1"
    exit 1
fi

if [ "$NIC" != "enp65s0f0np0" ] && [ "$NIC" != "enp23s0f0np0" ] && [ "$NIC" != "ens1f1np1" ]; then
    echo "Error: nic must be enp65s0f0np0 or enp23s0f0np0 or ens1f1np1"
    exit 1
fi

# echo "⭐️ env loaded:"
# echo "  NODE_ID=$NODE_ID"
# echo "  NIC=$NIC"
# echo "  NIC_PCI=$NIC_PCI"
# echo "  TMPDIR=$TMPDIR"
# echo "  MAX_VM_COUNT=$MAX_VM_COUNT"