#!/usr/bin/env bash
set -ex

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -f "$SCRIPT_DIR/user-datas/user-data-vm"*
mkdir -p "$SCRIPT_DIR/user-datas"
for i in {0..31}; do
  ROLE=$(if [ $((i % 2)) -eq 0 ]; then echo "server"; else echo "client"; fi)
  SERVER_IP="192.168.100.$((i+1))"
  cat > "$SCRIPT_DIR/user-datas/user-data-vm$i" <<EOF
#cloud-config
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
  - path: /etc/vm_role
    content: |
      VM_INDEX=$i
      ROLE=$ROLE
  - path: /etc/systemd/system/iperf.service
    permissions: '0644'
    content: |
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

  - path: /usr/local/bin/start-iperf.sh
    permissions: '0755'
    content: |
      #!/bin/bash
      set -e
      source /etc/vm_role
      
      log() {
          echo "[\$(date '+%Y-%m-%d %H:%M:%S')] vm\$VM_INDEX: \$1"
      }

      if [ "\$ROLE" = "server" ]; then
          log "starting iperf server"
          exec iperf3 -s
      else
          log "starting iperf client (target: $SERVER_IP)"
          
          # Wait for server to be ready
          sleep 5
          
          # Run iperf test once - systemd will restart it
          iperf3 -c $SERVER_IP -P 4 -t 30 -J \\
          | jq --arg vm "vm$i" '. + {vm: \$vm}' \\
          | nc -N 192.168.100.1 9000
          
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

EOF
done
