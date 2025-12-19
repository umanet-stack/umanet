#! /bin/bash

sudo apt update
sudo apt install -y flex bison libelf-dev mtools libguestfs-tools
sudo apt install -y doxygen graphviz
# sudo apt install -y dpdk dpdk-dev cpuset virtiofsd

# kernel
cd ~
git clone --depth 1 https://github.com/cloud-hypervisor/linux.git -b ch-6.12.8 linux-cloud-hypervisor
pushd linux-cloud-hypervisor
make ch_defconfig
KCFLAGS="-Wa,-mx86-used-note=no" make bzImage -j `nproc`
popd
mv ~/linux-cloud-hypervisor/arch/x86/boot/compressed/vmlinux.bin /tmp/vmlinux.bin

# image
cd ~
wget https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
qemu-img convert -p -f qcow2 -O raw noble-server-cloudimg-amd64.img noble-server-cloudimg-amd64.raw
mv noble-server-cloudimg-amd64.raw /tmp/noble-server-cloudimg-amd64.raw
