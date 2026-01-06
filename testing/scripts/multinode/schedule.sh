#!bin/sh

source env.sh

if [ "$NODE_ID" = "0" ]; then
  ./testing/scripts/schedule_client.sh
elif [ "$NODE_ID" = "1" ]; then
  ./testing/scripts/schedule_server.sh
fi
