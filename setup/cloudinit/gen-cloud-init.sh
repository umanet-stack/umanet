#!/usr/bin/env bash
set -ex

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <node_id> <num_vms>"
  echo "  node_id: 0 or 1"
  echo "  num_vms: number of VMs"
  echo "Example: $0 0 64"
  exit 1
fi

NODE_ID=$1
NUM_VMS=$2
# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

$SCRIPT_DIR/gen-network-config.sh $NODE_ID $NUM_VMS
$SCRIPT_DIR/gen-user-data.sh

# Function to create a cloud-init disk
create_iso() {
    local output="$1"
    local netconfig="$2"
    local userdata="$3"

    rm -f "${output}"
    
    # Create FAT filesystem with CIDATA label (NoCloud datasource looks for this)
    mkdosfs -n CIDATA -C "${output}" 8192
    
    # Copy files with correct names
    mcopy -oi "${output}" -s "${userdata}" ::user-data
    mcopy -oi "${output}" -s "${SCRIPT_DIR}/meta-data" ::meta-data
    mcopy -oi "${output}" "${netconfig}" ::network-config
}

# Create the ISOs
sudo rm -rf /tmp/cloudinit
mkdir -p /tmp/cloudinit
for ((i=0; i<NUM_VMS; i++)); do
  create_iso "/tmp/cloudinit/cloudinit-vm$i.img" "$SCRIPT_DIR/netplans/network-vm$i" "$SCRIPT_DIR/user-datas/user-data-vm"
done
