# buildroot
```bash
cd ~/code
git clone https://github.com/buildroot/buildroot.git
cd buildroot

git checkout 2025.08.x
make qemu_x86_64_defconfig

# opens gui, make sure terminal big enough
make menuconfig
# Target packages → Networking applications: iperf3, iproute2
# Filesystem images: ext2/3/4 root filesystem, ext4

make -j$(nproc)

mkdir -p /tmp/buildroot
cp ~/code/buildroot/output/images/bzImage /tmp/buildroot/vm0
cp ~/code/buildroot/output/images/rootfs.ext4 /tmp/buildroot/rootfs.ext4

sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/buildroot/vm0 \
  --cmdline "console=ttyS0 root=/dev/vda rw init=/sbin/init" \
  --disk path=/tmp/buildroot/rootfs.ext4 \
  --serial tty \
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048
```

# alpine
```bash
cd ~
wget https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/cloud/aws_alpine-3.23.0-x86_64-bios-tiny-r0.vhd

qemu-img convert -f vpc -O raw \
  ~/aws_alpine-3.23.0-x86_64-bios-tiny-r0.vhd \
  /tmp/alpine-tiny.raw


# For Alpine you must use a bzImage / vmlinuz, NOT vmlinux
sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /boot/vmlinuz-$(uname -r) \
  --cmdline "console=ttyS0 root=/dev/vda1 rw init=/sbin/init panic=1" \
  --disk path=/tmp/alpine-tiny.raw \
  --net mac=52:54:00:02:d9:02,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048 

sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/vmlinux.bin \
  --cmdline "console=ttyS0 root=/dev/vda1 rw init=/sbin/init panic=1" \
  --disk path=/tmp/alpine-tiny.raw \
  --net mac=52:54:00:02:d9:02,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048 

```