#!/bin/sh

now_epoch=$(date +%s)

for i in $(seq 1 32); do
  run_epoch=$(( now_epoch + i * 120 ))
  run_time=$(date -d "@$run_epoch" "+%H:%M")

  echo "$PWD/run_one_client.sh $i" | at "$run_time"
  echo "Schedule to run '$PWD/run_one_client.sh $i' at $run_time"
done
