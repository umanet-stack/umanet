#!/usr/bin/env bash

set -e

source env.sh

TESTNAME=ovsdpdk32vm
NUM_VM=32

DATA_DIR=~/code/umanet/testing/wrk
SCRPIT_DIR=~/code/umanet/testing/scripts/wrk

for R in $(ls $DATA_DIR/rates | sort -n); do
  RPS=$(awk -v n=$NUM_VM 'NR<=n{sum+=$0} END{print sum}' rates/$R)
  BASE_DIR="$DATA_DIR/out/$TESTNAME/$RPS"
  mkdir -p $BASE_DIR
  [ -f "$BASE_DIR/done" ] && echo "Found finished result $R, skipping" && continue
  echo "Running with R=$R"
  for i in $(seq 1 $NUM_VM); do
    echo "$SCRPIT_DIR/start.sh $R $i > $BASE_DIR/$i"
    tmux new-session -s "vm$i" -d "$SCRPIT_DIR/client-start.sh $R $i > $BASE_DIR/$i"
  done
  touch $BASE_DIR/done
  sleep 30
done

$SCRPIT_DIR/transform.pl 0.5
$SCRPIT_DIR/transform.pl 0.9
