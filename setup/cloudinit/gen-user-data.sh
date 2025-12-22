#!/usr/bin/env bash
set -ex

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -f "$SCRIPT_DIR/user-datas/user-data-vm"*
mkdir -p "$SCRIPT_DIR/user-datas"
cat > "$SCRIPT_DIR/user-datas/user-data-vm$i" <<EOF
#cloud-config
# Disable cloud-init network configuration - we configure network via netplan.service
network:
  config: disabled

packages:
  - iperf
  - iperf3
  - sockperf
users:
  - name: cloud
    passwd: \$6\$IwCereKq.VkDH2vr\$Kq.L80VAg5jMynEKwz61pcAjImBSAsE7AwwTiGe4qq.lmFzkOakSk4.BmbZ4ypCVALXwpVDlFpTN73TQ0jzXW. 
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: false
    inactive: false
    shell: /bin/bash

ssh_pwauth: true
chpasswd:
  expire: false

write_files:
  - path: /etc/systemd/resolved.conf.d/dns.conf
    content: |
      [Resolve]
      DNS=8.8.8.8 8.8.4.4
    permissions: '0644'

  - path: /etc/systemd/system/netplan.service
    permissions: '0644'
    content: |
      [Unit]
      Description=Inject Netplan Config from Cmdline
      DefaultDependencies=no
      After=systemd-modules-load.service systemd-udev-trigger.service
      Before=network-pre.target
      Wants=network-pre.target

      [Service]
      Type=oneshot
      ExecStart=/usr/local/bin/netplan-gen.sh
      RemainAfterExit=yes
      StandardOutput=journal+console
      StandardError=journal+console
      Environment="TMPDIR=/run"

      [Install]
      WantedBy=multi-user.target
    
  - path: /usr/local/bin/netplan-gen.sh
    permissions: '0755'
    content: |
      #!/bin/bash
      # Parse kernel command-line parameters
      for param in \$(cat /proc/cmdline); do
          case \$param in
              NETPLAN_CONFIG_B64=*) export NETPLAN_CONFIG_B64="\${param#NETPLAN_CONFIG_B64=}";;
          esac
      done

      log() {
          echo "[\$(date '+%Y-%m-%d %H:%M:%S')] vm: \$1"
      }

      # Decode the base64-encoded command
      if [ -n "\$NETPLAN_CONFIG_B64" ]; then
          export NETPLAN_CONFIG=\$(echo -n "\$NETPLAN_CONFIG_B64" | base64 -d)
      else
          log "NETPLAN_CONFIG_B64 is not set"
          exit 1
      fi
      
      log "injecting netplan config (\$NETPLAN_CONFIG)"
      # Write to /run/netplan with high priority (99-) to override cloud-init config from /etc
      # Netplan processes both /etc/netplan and /run/netplan, with lexicographic ordering
      # Note: systemd services run as root, so no sudo needed
      mkdir -p /run/netplan
      echo "\$NETPLAN_CONFIG" > /run/netplan/99-cmdline.yaml
      chmod 0644 /run/netplan/99-cmdline.yaml
      log "netplan config written to /run/netplan/99-cmdline.yaml"

      log "running netplan generate..."
      netplan generate
      log "running netplan apply..."
      netplan apply
      log "netplan applied successfully"
      
      # Verify the interface came up (wait a moment for it to configure)
      sleep 2
      if ip addr show | grep -q "192.168.100"; then
          log "Network interface configured successfully"
      else
          log "WARNING: Network interface may not be configured correctly"
          log "Current IP addresses:"
          ip addr show 2>&1 | while IFS= read -r line; do log "  \$line"; done
      fi

  - path: /etc/systemd/system/iperf.service
    permissions: '0644'
    content: |
      [Unit]
      Description=iperf role
      After=netplan.service network.target
      Wants=network.target

      [Service]
      Type=simple
      ExecStart=/usr/local/bin/start-iperf.sh
      Restart=no
      StandardOutput=journal+console
      StandardError=journal+console

      [Install]
      WantedBy=multi-user.target

  - path: /usr/local/bin/start-iperf.sh
    permissions: '0755'
    content: |
      #!/bin/bash
      # Parse kernel command-line parameters
      for param in \$(cat /proc/cmdline); do
          case \$param in
              ROLE=*) export ROLE="\${param#ROLE=}";;
              IPERF_COMMAND_B64=*) export IPERF_COMMAND_B64="\${param#IPERF_COMMAND_B64=}";;
          esac
      done
      
      # Decode the base64-encoded command
      if [ -n "\$IPERF_COMMAND_B64" ]; then
          export IPERF_COMMAND=\$(echo -n "\$IPERF_COMMAND_B64" | base64 -d)
      fi
      
      log() {
          echo "[\$(date '+%Y-%m-%d %H:%M:%S')] vm: \$1"
      }

      if [ "\$ROLE" = "server" ]; then
          log "starting iperf server (\$IPERF_COMMAND)"
          exec \$IPERF_COMMAND
      else
          log "starting iperf client (\$IPERF_COMMAND)"
          
          # Wait for server to be ready
          sleep 5
          
          # Run iperf test normally (shows progress in logs) and capture JSON output
          log "running iperf3 test..."
          IPERF_OUTPUT=\$(\$IPERF_COMMAND 2>&1)
          IPERF_EXIT=\$?
          
          if [ \$IPERF_EXIT -ne 0 ]; then
              log "ERROR: iperf3 failed with exit code \$IPERF_EXIT"
              log "iperf3 output: \$IPERF_OUTPUT"
              exit 1
          fi
          
          # Display summary from JSON output
          log "iperf3 test completed successfully"
          if command -v jq >/dev/null 2>&1; then
              SUMMARY=\$(echo "\$IPERF_OUTPUT" | jq -r '
                  "  Throughput: " + (.end.sum_sent.bits_per_second / 1e9 | tostring) + " Gbps | " +
                  "Bytes: " + (.end.sum_sent.bytes / 1e9 | tostring) + " GB | " +
                  "Retransmits: " + (.end.sum_sent.retransmits | tostring) + " | " +
                  "CPU (host): " + (.end.cpu_utilization_percent.host_total | tostring) + "% | " +
                  "CPU (remote): " + (.end.cpu_utilization_percent.remote_total | tostring) + "%"
              ' 2>/dev/null || echo "  (summary unavailable)")
              log "\$SUMMARY"
              
              # Log throughput per second for timeseries graph
              log "iperf3 intervals (throughput per second):"
              echo "\$IPERF_OUTPUT" | jq -r '.intervals[] | "  [" + (.sum.start | tostring) + "-" + (.sum.end | tostring) + "s] " + (.sum.bits_per_second / 1e9 | tostring) + " Gbps"' 2>/dev/null || true
          fi
          
          log "finished iperf client"
      fi

# Fix sudoers issues
runcmd:
  - rm -f /etc/sudoers.d/README
  - |
    # Remove null bytes from sudoers files
    for f in /etc/sudoers.d/*; do
      [ -f "\$f" ] && sed -i 's/\x00//g' "\$f"
    done
  - systemctl restart systemd-resolved
  - systemctl daemon-reexec
  - systemctl daemon-reload
  - systemctl enable iperf
  - systemctl start iperf
  - systemctl enable netplan
  - systemctl start netplan

EOF
