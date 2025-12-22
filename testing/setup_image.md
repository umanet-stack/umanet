# Setup CH image
```bash
./setup/download_img.sh
# Cloud-init will NOT run again on these images, it only runs on first boot.
# if you modify anything in cloud-init, you need to copy the image and run cloud-init on it again.
./setup/cloudinit/gen-cloud-init.sh

sudo rm -f /tmp/vm-img.raw
sudo cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm-img.raw

# base image: let it install packages + setup services
# cloudinit-vm.img has vm0's network config so that it can download packages
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--net "tap=tap0,mac=12:34:56:78:90:00" \
	--disk path=/tmp/vm-img.raw path=/tmp/cloudinit/cloudinit-vm.img

sudo cloud-init clean --logs
sudo touch /etc/cloud/cloud-init.disabled
sudo poweroff




```