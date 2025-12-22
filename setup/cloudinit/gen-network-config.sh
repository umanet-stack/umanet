#!/usr/bin/env bash
set -ex

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -f "$SCRIPT_DIR/netplans/network-vm"*
mkdir -p "$SCRIPT_DIR/netplans"
for i in {0..31}; do
  MAC_ADDRESS="12:34:56:78:90:$(printf "%02X" $i)"
  cat > "$SCRIPT_DIR/netplans/network-vm$i" <<EOF
network:
  version: 2
  ethernets:
    ens4:
      # apply this configuration to whichever network interface has the MAC address
      match:
        macaddress: $MAC_ADDRESS
      dhcp4: no
      addresses: [192.168.100.$((i+2))/24]
      routes:
        - to: default
          via: 192.168.100.1
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
      optional: true
EOF
done

rm -f "/tmp/netplans/network-vm"*
mkdir -p "/tmp/netplans"
cp "$SCRIPT_DIR/netplans/network-vm"* "/tmp/netplans"