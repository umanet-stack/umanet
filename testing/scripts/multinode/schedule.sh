#!/bin/bash

source env.sh

if [ "$NODE_ID" = "0" ]; then
  ./testing/scripts/multinode/schedule_client.sh
elif [ "$NODE_ID" = "1" ]; then
  ./testing/scripts/multinode/schedule_server.sh
fi
