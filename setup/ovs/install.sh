#!/bin/bash
set -e

source env.sh

echo "[1/6] Install dependencies"
sudo apt update
sudo apt install -y \
  pkg-config build-essential libnuma-dev python3-pyelftools \
  libelf-dev libarchive-dev ninja-build meson \
  libssl-dev libcap-ng-dev libunbound-dev \
  linux-headers-$(uname -r) \
  autoconf automake libtool \
  wget curl net-tools sparse \
  rdma-core ibverbs-providers libibverbs-dev libpcap-dev \
  libsystemd-dev \
  qemu-kvm libvirt-daemon-system libvirt-clients \
  virtinst bridge-utils \
  flex bison

sudo chown $(id -u):$(id -g) /usr/src

# ------------------------------------------------------------

echo "[2/6] Build & install DPDK 25.11"
cd /usr/src
wget -nc https://fast.dpdk.org/rel/dpdk-25.11.tar.xz
tar xf dpdk-25.11.tar.xz

export DPDK_DIR=/usr/src/dpdk-25.11
export DPDK_PREFIX=/usr/local/dpdk

cd $DPDK_DIR
meson setup build --prefix=$DPDK_PREFIX
ninja -C build
sudo ninja -C build install
sudo ldconfig

# ------------------------------------------------------------

echo "[3/6] Build & install Open vSwitch 3.6.1 (DPDK)"
cd /usr/src
git clone https://github.com/openvswitch/ovs.git --branch v3.6.1 --depth=1
cd ovs

./boot.sh
export PKG_CONFIG_PATH=$DPDK_PREFIX/lib/pkgconfig:$DPDK_PREFIX/lib/x86_64-linux-gnu/pkgconfig
./configure --with-dpdk=shared CFLAGS="-O3 -march=native"
make -j$(nproc)
sudo make install
sudo ldconfig
