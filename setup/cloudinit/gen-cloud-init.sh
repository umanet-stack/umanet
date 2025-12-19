#!/usr/bin/env bash
set -ex

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

$SCRIPT_DIR/gen-network-config.sh
$SCRIPT_DIR/gen-user-data.sh

# Function to create a cloud-init ISO
create_iso() {
    local output="$1"
    local netconfig="$2"

    rm -f "${output}"
    mkdosfs -n CIDATA -C "${output}" 8192
    mcopy -oi "${output}" -s "${SCRIPT_DIR}/user-data" ::
    mcopy -oi "${output}" -s "${SCRIPT_DIR}/meta-data" ::
    # Copy network config and rename it to "network-config" (cloud-init expects this name)
    mcopy -oi "${output}" "${netconfig}" ::network-config
}

# Create the ISOs
sudo rm -rf /tmp/cloudinit
mkdir -p /tmp/cloudinit
for i in {0..31}; do
  create_iso "/tmp/cloudinit/cloudinit-vm$i.img" "$SCRIPT_DIR/network-configs/network-vm$i"
done
