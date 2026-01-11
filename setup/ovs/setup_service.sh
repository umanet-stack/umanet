#!/bin/bash
set -e

source env.sh

# ------------------------------------------------------------

echo "[1/4] Disable kernel Open vSwitch permanently"
sudo systemctl disable --now openvswitch-switch 2>/dev/null || true
sudo systemctl disable --now openvswitch 2>/dev/null || true
sudo modprobe -r openvswitch 2>/dev/null || true

# ------------------------------------------------------------

echo "[2/4] Configure hugepages for DPDK"
# Allocate hugepages (default: 24576 × 2MB = 48GB, but use a reasonable default if not set)
NR_HUGEPAGES=${NR_HUGEPAGES:-24576}
sudo sysctl -w vm.nr_hugepages=$NR_HUGEPAGES

# Mount hugepage filesystem if not already mounted
if ! mountpoint -q /mnt/huge 2>/dev/null; then
    sudo mkdir -p /mnt/huge
    sudo mount -t hugetlbfs nodev /mnt/huge || true
fi

# Also ensure /dev/hugepages exists (DPDK may use either location)
if ! mountpoint -q /dev/hugepages 2>/dev/null; then
    sudo mkdir -p /dev/hugepages
    sudo mount -t hugetlbfs nodev /dev/hugepages || true
fi

echo "✅ Hugepages configured: $(grep HugePages_Total /proc/meminfo | awk '{print $2}') pages"

# ------------------------------------------------------------

echo "[3/4] Install OVS-DPDK systemd service"

# Create systemd slice if it doesn't exist
if [ ! -f /etc/systemd/system/ovs.slice ]; then
    sudo tee /etc/systemd/system/ovs.slice >/dev/null << 'EOF'
[Unit]
Description=Open vSwitch slice
[Slice]
CPUAccounting=yes
EOF
fi

sudo tee /etc/systemd/system/ovs-dpdk.service >/dev/null << 'EOF'
[Unit]
Description=Open vSwitch with DPDK
After=network.target
Wants=network.target
RequiresMountsFor=/mnt/huge /dev/hugepages

[Service]
Type=forking
CPUAffinity=0-7
Slice=ovs.slice
CPUAccounting=yes

# Ensure hugepages are available
ExecStartPre=/bin/sh -c 'if [ "$(grep HugePages_Free /proc/meminfo | awk "{print \$2}")" -lt 512 ]; then echo "Error: Not enough hugepages available"; exit 1; fi'

ExecStartPre=/bin/mkdir -p /usr/local/etc/openvswitch
ExecStartPre=/bin/mkdir -p /usr/local/var/run/openvswitch
ExecStartPre=/bin/mkdir -p /usr/local/var/log/openvswitch

ExecStartPre=/bin/sh -c '/usr/bin/test -f /usr/local/etc/openvswitch/conf.db || /usr/local/bin/ovsdb-tool create /usr/local/etc/openvswitch/conf.db /usr/local/share/openvswitch/vswitch.ovsschema'

ExecStart=/usr/local/sbin/ovsdb-server \
  --remote=punix:/usr/local/var/run/openvswitch/db.sock \
  --remote=db:Open_vSwitch,Open_vSwitch,manager_options \
  --pidfile=/usr/local/var/run/openvswitch/ovsdb-server.pid \
  --detach

# Wait for ovsdb-server to be ready before starting ovs-vswitchd
ExecStartPost=/bin/sleep 2
ExecStartPost=/bin/sh -c 'timeout=30; while [ ! -S /usr/local/var/run/openvswitch/db.sock ] && [ $timeout -gt 0 ]; do sleep 0.1; timeout=$((timeout-1)); done; if [ ! -S /usr/local/var/run/openvswitch/db.sock ]; then exit 1; fi'

# Configure DPDK settings in OVS database before starting ovs-vswitchd
ExecStartPost=/bin/sh -c '/usr/local/bin/ovs-vsctl --no-wait --db=unix:/usr/local/var/run/openvswitch/db.sock set Open_vSwitch . other_config:dpdk-init=true || true'
ExecStartPost=/bin/sh -c '/usr/local/bin/ovs-vsctl --no-wait --db=unix:/usr/local/var/run/openvswitch/db.sock set Open_vSwitch . other_config:dpdk-socket-mem=1024 || true'
ExecStartPost=/bin/sh -c '/usr/local/bin/ovs-vsctl --no-wait --db=unix:/usr/local/var/run/openvswitch/db.sock set Open_vSwitch . other_config:dpdk-lcore-mask=0xf || true'

# Start ovs-vswitchd (it will read DPDK config from database)
ExecStartPost=/usr/local/sbin/ovs-vswitchd \
  unix:/usr/local/var/run/openvswitch/db.sock \
  --mlockall \
  --no-chdir \
  --log-file=/usr/local/var/log/openvswitch/ovs-vswitchd.log \
  --pidfile=/usr/local/var/run/openvswitch/ovs-vswitchd.pid \
  --detach

# Wait a bit to ensure ovs-vswitchd started successfully
ExecStartPost=/bin/sleep 1
ExecStartPost=/bin/sh -c 'if ! pgrep -f ovs-vswitchd > /dev/null; then echo "Error: ovs-vswitchd failed to start"; exit 1; fi'

ExecStop=/bin/sh -c '/usr/bin/pkill ovs-vswitchd || true'
ExecStopPost=/bin/sh -c '/usr/bin/pkill ovsdb-server || true'
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable ovs-dpdk

# ------------------------------------------------------------

echo "[4/4] Done. Reboot recommended."
