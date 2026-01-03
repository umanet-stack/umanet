#!/bin/bash

cd ~/code/umanet/setup/img
sudo rm -rf initramfs || true
sudo rm -f /tmp/initramfs-overlay.img || true

mkdir -p initramfs/{bin,sbin,proc,sys,dev}
cp /bin/busybox initramfs/bin/
ln -s busybox initramfs/bin/sh

cat > initramfs/init << 'EOF'
#!/bin/sh
set -eux

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

mkdir -p /lower /upper /newroot
mount -o ro /dev/vda1 /lower
mount /dev/vdb /upper

mkdir -p /upper/upper /upper/work
mount -t overlay overlay \
  -o lowerdir=/lower,upperdir=/upper/upper,workdir=/upper/work \
  /newroot

exec switch_root /newroot /sbin/init
EOF

chmod +x initramfs/init

cd initramfs
chmod +x bin/busybox
sudo chroot . /bin/busybox --install -s
ls bin/mount sbin/switch_root bin/sh

find . | cpio -H newc -o | gzip > /tmp/initramfs-overlay.img
