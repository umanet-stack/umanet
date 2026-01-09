#!/usr/bin/env bash

set -e

source env.sh

if [ "$NODE_ID" = "0" ]; then
  ./testing/scripts/multinode/run_all_client.sh
elif [ "$NODE_ID" = "1" ]; then
  ./testing/scripts/multinode/run_all_server.sh
fi
