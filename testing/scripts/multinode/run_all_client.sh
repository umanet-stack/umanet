#!/usr/bin/env bash
set -e
i=1
while [ "$i" -le 32 ]; do
  while (( SECONDS % 100 != 20 )); do
    if (( SECONDS % 10 == 0 )); then
      echo "Starting in $(( 100 - ((SECONDS - 20 + 100) % 100) )) seconds..."
    fi
    sleep 1
  done
  ./testing/scripts/multinode/run_one_client.sh $i
  ((i++))
done
