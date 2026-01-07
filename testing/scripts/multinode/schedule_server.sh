#!/bin/bash

now_epoch=$(date +%s)

for i in $(seq 1 32); do
  run_epoch=$(( now_epoch + i * 120 ))
  run_time=$(date -d "@$run_epoch" "+%H:%M")

  echo "$PWD/testing/scripts/multinode/run_one_server.sh $i" | at "$run_time"
  echo "Schedule to run '$PWD/testing/scripts/multinode/run_one_server.sh $i' at $run_time"
done
