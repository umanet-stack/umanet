#!/usr/bin/env bash

set -e

source env.sh

ID=$2
R=$(sed -n "${ID}p" ./testing/wrk/rates/$1)

if [ "$NETWORK" = "ovs-dpdk" ]; then
  TARGET_IP="192.168.100.$(( 2 * ID + 1 ))"
elif [ "$NETWORK" = "tap" -o "$NETWORK" = "dpdk" ]; then
  TARGET_IP="192.168.100.$(( ID + 1 ))"
else
  echo "Invalid network: $NETWORK"
  exit 1
fi

wrk -d20s -R$R -L http://$TARGET_IP
