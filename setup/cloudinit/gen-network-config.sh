#!/usr/bin/env bash
set -eu
source env.sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <num_vms>"
  echo "  num_vms: number of VMs"
  echo "Example: $0 64"
  exit 1
fi

NUM_VMS=$1
# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -f "$SCRIPT_DIR/netplans/network-vm"*
mkdir -p "$SCRIPT_DIR/netplans"
for ((i=0; i<NUM_VMS; i++)); do
  MAC_ADDRESS="${NODE_ID}2:34:56:78:90:$(printf "%02X" $i)"
  MAC_ADDRESS_2="${NODE_ID}2:34:56:78:91:$(printf "%02X" $i)"
  MAC_ADDRESS_OVS="${NODE_ID}2:34:56:78:92:$(printf "%02X" $i)"
  cat > "$SCRIPT_DIR/netplans/network-vm$i" <<EOF
version: 2
ethernets:
  ens4:
    # apply this configuration to whichever network interface has the MAC address
    match:
      macaddress: $MAC_ADDRESS
    dhcp4: no
    addresses: [192.168.10${NODE_ID}.$((i+2))/24]
    routes:
      - to: default
        via: 192.168.10${NODE_ID}.1
    nameservers:
      addresses: [8.8.8.8, 8.8.4.4]
    mtu: 9000
    optional: true
  
  ens5:
    match:
      macaddress: $MAC_ADDRESS_2
    dhcp4: no
    addresses: [10.10.$((NODE_ID+1)).$((i+2))/24]
    routes:
      - to: 10.10.0.0/16
        via: 10.10.$((NODE_ID+1)).1
    nameservers:
      addresses: [8.8.8.8, 8.8.4.4]
    optional: true
  
  ens6:
    match:
      macaddress: $MAC_ADDRESS_OVS
    dhcp4: no
    addresses: [192.168.100.$(( NODE_ID + 2 + i * 2 ))/24]
    optional: true
EOF
done
# OVS node0: .2, .4, .6, ...
# OVS node1: .3, .5, .7, ...

# default via 192.168.x.1 because for TAP, this is br0
# 10.10.x.1 is br0 for TAP when it is dpdk mode (same-node network only)