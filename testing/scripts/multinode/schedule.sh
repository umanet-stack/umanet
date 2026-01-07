#!/bin/bash

set -e

source env.sh

[ command -v at ] || echo "at is not installed plase run 'sudo apt install -y at'"

if [ "$NODE_ID" = "0" ]; then
  ./testing/scripts/multinode/schedule_client.sh
elif [ "$NODE_ID" = "1" ]; then
  ./testing/scripts/multinode/schedule_server.sh
fi
