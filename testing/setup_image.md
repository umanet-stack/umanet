# Setup CH image
```bash
./setup/download_img.sh
# base read-only image (ubuntu cloudimg)
sudo rm -f /tmp/vm-img.raw
sudo cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm-img.raw

# overlay fs + rw disks
./setup/img/build_initramfs.sh
./setup/img/build_rw_disk.sh 32 512

# Cloud-init will NOT run again on these rw disks, it only runs on first boot.
# if you modify anything in cloud-init, you need to regen the rw disks and run cloud-init on it again.
./setup/cloudinit/gen-cloud-init.sh

# first run: let it install packages + setup services (use tap to access internet)
./setup/tap/setup_br_tap.sh 0
./setup/tap/spawn_vms.sh 32 /tmp samenode

# kill all vms when done
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

## Testing
```bash
# vm0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-0.img path=/tmp/cloudinit/cloudinit-vm0.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap0,mac=12:34:56:78:90:00"

# vm1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--initramfs /tmp/initramfs-overlay.img \
	--disk path=/tmp/vm-img.raw,readonly=on path=/tmp/disks/state-1.img path=/tmp/cloudinit/cloudinit-vm1.img \
	--cmdline "console=ttyS0 console=hvc0 rdinit=/init systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap1,mac=12:34:56:78:90:01"
```