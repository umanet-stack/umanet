#!/bin/bash
set -e

source env.sh

# ------------------------------------------------------------

echo "[1/3] Disable kernel Open vSwitch permanently"
sudo systemctl disable --now openvswitch-switch 2>/dev/null || true
sudo systemctl disable --now openvswitch 2>/dev/null || true
sudo modprobe -r openvswitch 2>/dev/null || true

# ------------------------------------------------------------

echo "[2/3] Install OVS-DPDK systemd service"

sudo tee /etc/systemd/system/ovs-dpdk.service >/dev/null << 'EOF'
[Unit]
Description=Open vSwitch with DPDK
After=network.target
Wants=network.target

[Service]
Type=forking
CPUAffinity=0-15
Slice=ovs.slice
CPUAccounting=yes

ExecStartPre=/bin/mkdir -p /usr/local/etc/openvswitch
ExecStartPre=/bin/mkdir -p /usr/local/var/run/openvswitch
ExecStartPre=/bin/mkdir -p /usr/local/var/log/openvswitch

ExecStartPre=/bin/sh -c '/usr/bin/test -f /usr/local/etc/openvswitch/conf.db || /usr/local/bin/ovsdb-tool create /usr/local/etc/openvswitch/conf.db /usr/local/share/openvswitch/vswitch.ovsschema'

ExecStart=/usr/local/sbin/ovsdb-server \
  --remote=punix:/usr/local/var/run/openvswitch/db.sock \
  --remote=db:Open_vSwitch,Open_vSwitch,manager_options \
  --pidfile \
  --detach

ExecStartPost=/usr/local/sbin/ovs-vswitchd \
  unix:/usr/local/var/run/openvswitch/db.sock \
  --mlockall \
  --no-chdir \
  --log-file=/usr/local/var/log/openvswitch/ovs-vswitchd.log \
  --pidfile=/usr/local/var/run/openvswitch/ovs-vswitchd.pid \
  --detach

ExecStop=/usr/bin/pkill ovs-vswitchd
ExecStopPost=/usr/bin/pkill ovsdb-server
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable ovs-dpdk

# ------------------------------------------------------------

echo "[3/3] Done. Reboot recommended."
