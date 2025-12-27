#!/usr/bin/env bash
set -a          # automatically export all variables
source .env     # load key=value pairs
set +a

# Validation (fail fast)
: "${NODE_ID:?missing NODE_ID}"
: "${NIC:?missing NIC}"
: "${NIC_PCI:?missing NIC_PCI}"
: "${TMPDIR:?missing TMPDIR}"

VM_COUNT=32
NETWORK=tap
TEST_MODE=vm-vm-internal

echo "⭐️ env loaded:"
echo "  NODE_ID=$NODE_ID"
echo "  NIC=$NIC"
echo "  NIC_PCI=$NIC_PCI"
echo "  TMPDIR=$TMPDIR"
