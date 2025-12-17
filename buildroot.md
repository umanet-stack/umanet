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


sudo cloud-hypervisor \
  --cpus boot=1 \
  --memory size=512M,hugepages=on,shared=true \
  --kernel /tmp/buildroot/vm0 \
  --cmdline "console=ttyS0 root=/dev/vda rw" \
  --disk path=/tmp/noble-server-cloudimg-amd64.raw path=/tmp/cloudinit-vm0-dpdk.img \
  --net mac=52:54:00:02:d9:01,vhost_user=true,socket=/mnt/huge/sock0,num_queues=2,vhost_mode=client,queue_size=2048
```