#!/usr/bin/env bash
set -e

# Usage: ./setup_vtap.sh
# Creates a real TAP device (vtap0) that DPDK can attach to,
# then bridges it to br0 via veth pair

echo "Setting up TAP device for DPDK <-> br0 communication..."

# Delete existing vtap0 (TAP) and bridge setup
sudo ip link delete vtap0 2>/dev/null || true
sudo ip link delete veth0 2>/dev/null || true
echo "✅ Cleaned up old interfaces"

# Create a real TAP device that DPDK will attach to
sudo ip tuntap add dev vtap0 mode tap
sudo ip link set vtap0 up
echo "✅ Created TAP device: vtap0"

# Create veth pair to bridge vtap0 to br0
# vtap0 (TAP for DPDK) -> veth0 (in same namespace) -> veth1 (attached to br0)
# Actually, we can directly add vtap0 to br0!
sudo ip link set vtap0 master br0
echo "✅ Attached vtap0 directly to br0"

# Verify
echo ""
echo "Verification:"
ip link show vtap0
bridge link show | grep vtap0
echo ""
echo "✅ Setup complete. DPDK app can now attach to vtap0 and forward packets to br0"
