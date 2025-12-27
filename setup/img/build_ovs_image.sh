#!/usr/bin/env bash
set -ex

# Script to prepare VM base image using virt-customize and virt-sysprep
# This replaces cloud-init for package installation and basic configuration

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <base_image_path>"
  echo "  base_image_path: path to the base raw disk image (e.g., /tmp/noble-server-cloudimg-amd64.raw)"
  exit 1
fi

BASE_IMAGE="$1"
OUTPUT_IMAGE="${BASE_IMAGE%.raw}-customized.raw"

qemu-img convert -f raw -O raw $BASE_IMAGE $OUTPUT_IMAGE

# Create temporary directory for files to inject
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Create DNS config file
mkdir -p "$TMP_DIR/etc/systemd/resolved.conf.d"
cat > "$TMP_DIR/etc/systemd/resolved.conf.d/dns.conf" <<'EOF'
[Resolve]
DNS=8.8.8.8 8.8.4.4
EOF

# Create iperf systemd service file
mkdir -p "$TMP_DIR/etc/systemd/system"
cat > "$TMP_DIR/etc/systemd/system/iperf.service" <<'EOF'
[Unit]
Description=iperf role
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/start-iperf.sh
Restart=no
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF

# Create start-iperf.sh script
mkdir -p "$TMP_DIR/usr/local/bin"
cat > "$TMP_DIR/usr/local/bin/start-iperf.sh" <<'EOF'
#!/bin/bash
# Parse kernel command-line parameters
for param in $(cat /proc/cmdline); do
    case $param in
        ROLE=*) export ROLE="${param#ROLE=}";;
        IPERF_COMMAND_B64=*) export IPERF_COMMAND_B64="${param#IPERF_COMMAND_B64=}";;
    esac
done

# Decode the base64-encoded command
if [ -n "$IPERF_COMMAND_B64" ]; then
    export IPERF_COMMAND=$(echo -n "$IPERF_COMMAND_B64" | base64 -d)
fi

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] vm: $1"
}

if [ "$ROLE" = "server" ]; then
    log "starting iperf server ($IPERF_COMMAND)"
    exec $IPERF_COMMAND
else
    log "starting iperf client ($IPERF_COMMAND)"

    # Wait for server to be ready
    sleep 5

    # Run iperf test normally (shows progress in logs) and capture JSON output
    log "running iperf3 test..."
    IPERF_OUTPUT=$($IPERF_COMMAND 2>&1)
    IPERF_EXIT=$?

    if [ $IPERF_EXIT -ne 0 ]; then
        log "ERROR: iperf3 failed with exit code $IPERF_EXIT"
        log "iperf3 output: $IPERF_OUTPUT"
        exit 1
    fi

    # Display summary from JSON output
    log "iperf3 test completed successfully"
    if command -v jq >/dev/null 2>&1; then
        SUMMARY=$(echo "$IPERF_OUTPUT" | jq -r '
            "  Throughput: " + (.end.sum_sent.bits_per_second / 1e9 | tostring) + " Gbps | " +
            "Bytes: " + (.end.sum_sent.bytes / 1e9 | tostring) + " GB | " +
            "Retransmits: " + (.end.sum_sent.retransmits | tostring) + " | " +
            "CPU (host): " + (.end.cpu_utilization_percent.host_total | tostring) + "% | " +
            "CPU (remote): " + (.end.cpu_utilization_percent.remote_total | tostring) + "%"
        ' 2>/dev/null || echo "  (summary unavailable)")
        log "$SUMMARY"

        # Log throughput per second for timeseries graph
        log "iperf3 intervals (throughput per second):"
        echo "$IPERF_OUTPUT" | jq -r '.intervals[] | "  [" + (.sum.start | tostring) + "-" + (.sum.end | tostring) + "s] " + (.sum.bits_per_second / 1e9 | tostring) + " Gbps"' 2>/dev/null || true
    fi

    log "finished iperf client"
fi
EOF

# Create network configuration script that runs at boot
cat > "$TMP_DIR/usr/local/bin/configure-network.sh" <<'EOF'
#!/bin/bash
# Configure network based on VM_INDEX from kernel command line
# Falls back to MAC address parsing if VM_INDEX is not available
# This script is called by systemd at boot

# Parse kernel command-line parameters to get VM_INDEX
VM_INDEX=""
for param in $(cat /proc/cmdline); do
    case $param in
        VM_INDEX=*) VM_INDEX="${param#VM_INDEX=}";;
    esac
done

# If VM_INDEX not found in kernel cmdline, try to get it from MAC address
if [ -z "$VM_INDEX" ]; then
    # Wait for network interfaces to be available
    max_attempts=30
    attempt=0
    while [ $attempt -lt $max_attempts ]; do
        if [ -d /sys/class/net ]; then
            break
        fi
        sleep 1
        attempt=$((attempt + 1))
    done

    # Find the network interface with our MAC pattern (12:34:56:78:90:XX)
    for iface in /sys/class/net/*; do
        ifname=$(basename "$iface")
        # Skip loopback
        if [ "$ifname" = "lo" ]; then
            continue
        fi

        mac=$(cat "$iface/address" 2>/dev/null || echo "")

        if [[ "$mac" =~ ^12:34:56:78:90: ]]; then
            # Extract VM index from MAC address (last byte)
            VM_INDEX=$(echo "$mac" | cut -d: -f6)
            VM_INDEX=$((0x$VM_INDEX))
            break
        fi
    done
fi

# Validate VM_INDEX
if [ -z "$VM_INDEX" ] || ! [[ "$VM_INDEX" =~ ^[0-9]+$ ]]; then
    echo "ERROR: Could not determine VM_INDEX from kernel cmdline or MAC address"
    exit 1
fi

# Calculate IP address (10.10.1.$(vm_index+10))
ip_addr="10.10.1.$((VM_INDEX + 10))"

# Wait for network interfaces to be available and find the non-loopback interface
max_attempts=30
attempt=0
ifname=""
while [ $attempt -lt $max_attempts ]; do
    for iface in /sys/class/net/*; do
        candidate=$(basename "$iface")
        if [ "$candidate" != "lo" ] && [ -f "$iface/address" ]; then
            ifname="$candidate"
            break
        fi
    done
    if [ -n "$ifname" ]; then
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done

if [ -z "$ifname" ]; then
    echo "ERROR: Could not find network interface"
    exit 1
fi

# Create netplan config
mkdir -p /etc/netplan
cat > "/etc/netplan/50-vm-config.yaml" <<NETPLAN_EOF
network:
  version: 2
  ethernets:
    $ifname:
      dhcp4: no
      addresses: [$ip_addr/24]
      routes:
        - to: default
          via: 10.10.1.2
      nameservers:
        addresses: [8.8.8.8, 8.8.4.4]
      optional: true
NETPLAN_EOF

# Apply netplan configuration
netplan apply

echo "Configured network interface $ifname with IP $ip_addr (VM_INDEX=$VM_INDEX)"
EOF

# Create systemd service for network configuration
cat > "$TMP_DIR/etc/systemd/system/configure-network.service" <<'EOF'
[Unit]
Description=Configure VM Network
After=network-pre.target
Before=network-online.target
Wants=network-pre.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/configure-network.sh
RemainAfterExit=yes
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF

# Use virt-customize to modify the image
sudo virt-customize -a "$OUTPUT_IMAGE" \
    --root-password password:123456 \
    --run-command 'mkdir -p /etc/systemd/system' \
    --copy-in "$TMP_DIR/etc/systemd/system/iperf.service:/etc/systemd/system/" \
    --run-command 'mkdir -p /usr/local/bin' \
    --copy-in "$TMP_DIR/usr/local/bin/start-iperf.sh:/usr/local/bin/" \
    --run-command 'chmod +x /usr/local/bin/start-iperf.sh' \
    --copy-in "$TMP_DIR/usr/local/bin/configure-network.sh:/usr/local/bin/" \
    --run-command 'chmod +x /usr/local/bin/configure-network.sh' \
    --copy-in "$TMP_DIR/etc/systemd/system/configure-network.service:/etc/systemd/system/" \
    --run-command 'systemctl enable configure-network.service' \
    --run-command 'systemctl enable iperf.service' \
    --run-command 'systemctl enable ssh' \
    --run-command 'systemctl disable cloud-init 2>/dev/null || true' \
    --run-command 'systemctl disable cloud-init-local 2>/dev/null || true' \
    --run-command 'systemctl disable cloud-config 2>/dev/null || true' \
    --run-command 'systemctl disable cloud-final 2>/dev/null || true' \
    --run-command 'systemctl mask cloud-init 2>/dev/null || true' \
    --run-command 'systemctl mask cloud-init-local 2>/dev/null || true' \
    --run-command 'systemctl mask cloud-config 2>/dev/null || true' \
    --run-command 'systemctl mask cloud-final 2>/dev/null || true' \
    --run-command "systemctl disable snapd.service" \
    --run-command "systemctl disable snapd.socket" \
    --run-command "systemctl disable snapd.seeded.service" \
    --run-command "systemctl mask snapd.service" \
    --run-command "systemctl mask snapd.socket" \
    --run-command "systemctl mask snapd.seeded.service" \
    --copy-in $(which iperf3):/usr/bin

echo "Image prepared successfully: $OUTPUT_IMAGE"
