## Changing password
```bash
openssl passwd -6

```
## Testing img
```bash
cp /tmp/noble-server-cloudimg-amd64.raw /tmp/vm0-img.raw
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vmlinux.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/tmp/vm0-img.raw path=/tmp/cloudinit-vm0.img \
	--net "tap=tap0,mac=52:54:00:02:d9:01" 

```