#!/usr/bin/env bash
set -e

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <base_dir> <network>"
  exit 1
fi

BASE_DIR=$1
NETWORK=$2

if [ "$NETWORK" != "tap" ] && [ "$NETWORK" != "dpdk" ] && [ "$NETWORK" != "ovs-dpdk" ]; then
  echo "Error: network must be tap, dpdk, or ovs-dpdk"
  exit 1
fi

if [ "$NETWORK" = "tap" ]; then
    ${BASE_DIR}/setup/cpu/slice_cpu.sh tap
    ${BASE_DIR}/setup/vm/setup_br_tap.sh 32

elif [ "$NETWORK" = "dpdk" ]; then
    ${BASE_DIR}/setup/cpu/slice_cpu.sh dpdk

elif [ "$NETWORK" = "ovs-dpdk" ]; then
    ${BASE_DIR}/setup/cpu/slice_cpu.sh dpdk
fi