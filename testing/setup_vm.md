# Setup CH image
```bash
./setup/img/download_img.sh
# base read-only image (ubuntu cloudimg)
sudo rm -f /tmp/vm-img.raw
sudo cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm-img.raw

# overlay fs + rw disks
./setup/img/build_initramfs.sh
./setup/img/build_rw_disk.sh 32 512

# Cloud-init will NOT run again on these rw disks, it only runs on first boot.
# if you modify anything in cloud-init, you need to regen the rw disks and run cloud-init on it again.
./setup/cloudinit/gen-cloud-init.sh 32

# setup node (allow internet NAT)
./setup/setup_node.sh

sudo ./setup/ovs/install.sh
sudo ./setup/ovs/setup_service.sh
# first run: let it install packages + setup services (use tap to access internet)
./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-vm-internal

# kill all vms when done
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

## Testing TAP
```bash
# vm0 TAP node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap0,mac=02:34:56:78:90:00"

# vm1 TAP node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap1,mac=02:34:56:78:90:01"

# vm0 TAP node 1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap0,mac=12:34:56:78:90:00"
```

## Testing DPDK
make sure to run as TAP at least once to download iperf
```bash
# vm0 DPDK node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net tap=tap0,mac=02:34:56:78:91:00 mac=02:34:56:78:90:00,vhost_user=on,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=4096

# vm1 DPDK node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net tap=tap1,mac=02:34:56:78:91:01 mac=02:34:56:78:90:01,vhost_user=on,socket=/mnt/huge/sock1,num_queues=2,vhost_mode=client,queue_size=4096

# vm0 DPDK node 1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net tap=tap0,mac=12:34:56:78:91:00 mac=12:34:56:78:90:00,vhost_user=on,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=4096
```


## Testing OvS-DPDK
```bash
# vm0 OvS-DPDK node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net mac=02:34:56:78:92:00,vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user0,num_queues=2,vhost_mode=client,queue_size=4096

# vm1 OvS-DPDK node 0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net mac=02:34:56:78:92:01,vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user1,num_queues=2,vhost_mode=client,queue_size=4096

# vm0 OvS-DPDK node 1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net mac=12:34:56:78:92:00,vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user0,num_queues=2,vhost_mode=client,queue_size=4096
```
### Large VMs
```bash
# LARGE vm0 OvS-DPDK node 0
sudo cloud-hypervisor \
	--cpus boot=8 \
	--memory size=2048M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net mac=02:34:56:78:92:00,vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user0,num_queues=8,vhost_mode=client,queue_size=4096

# LARGE vm0 OvS-DPDK node 1
sudo cloud-hypervisor \
	--cpus boot=8 \
	--memory size=2048M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net mac=12:34:56:78:92:00,vhost_user=on,socket=/usr/local/var/run/openvswitch/vhost-user0,num_queues=8,vhost_mode=client,queue_size=4096
```