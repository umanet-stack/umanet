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
# c6620
./setup/setup_node.sh 0 enp23s0f0np0
# c6525-25g
./setup/setup_node.sh 0 enp65s0f0np0
# xl170
./setup/setup_node.sh 0 ens1f1np1

# disable SMT (2 threads/core => 1 thread/core)
echo off | sudo tee /sys/devices/system/cpu/smt/control

# first run: let it install packages + setup services (use tap to access internet)
./setup/vm/setup_br_tap.sh 0 enp23s0f0np0 32
./setup/vm/spawn_vms.sh tap 32 /tmp vm-vm-internal

# kill all vms when done
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

## Testing TAP
```bash
# vm0 TAP
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap0,mac=12:34:56:78:90:00"

# vm1 TAP
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap1,mac=12:34:56:78:90:01"
```

## Testing DPDK
make sure to run as TAP at least once to download iperf
```bash
# vm0 DPDK
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=true \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net tap=tap0,mac=12:34:56:78:91:00 mac=12:34:56:78:90:00,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=4096

# vm1 DPDK
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M,hugepages=on,shared=on \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net tap=tap1,mac=12:34:56:78:91:01 mac=12:34:56:78:90:01,vhost_user=true,socket=/mnt/huge/sock1,num_queues=2,vhost_mode=client,queue_size=4096
```