#! /bin/bash

# meson is new Makefile/CMake
# ninja is new make
sudo apt update
sudo apt install -y meson-1.5 build-essential libnuma-dev ninja-build nasm libibverbs-dev ibverbs-providers rdma-core python3-pip python3-venv

# intel-ipsec-mb downgrade (DPDK 19.11 needs v0.54, not v1.5+)
# Remove newer incompatible version
sudo apt remove -y libipsec-mb-dev libipsec-mb1

# Build and install v0.54 from source
cd ~ && git clone https://github.com/intel/intel-ipsec-mb.git
cd ~/intel-ipsec-mb && git checkout v0.54

# Build and install shared library
sudo make -j$(nproc) && sudo make install

# Build static library (needed for DPDK)
sudo make SHARED=n

# Install to locations where DPDK expects to find it
sudo cp ~/intel-ipsec-mb/libIPSec_MB.a /usr/lib/x86_64-linux-gnu/
sudo ln -s /usr/lib/libIPSec_MB.so.0.54.0 /usr/lib/x86_64-linux-gnu/libIPSec_MB.so.0.54.0
sudo ln -s /usr/lib/x86_64-linux-gnu/libIPSec_MB.so.0.54.0 /usr/lib/x86_64-linux-gnu/libIPSec_MB.so.0
sudo ln -s /usr/lib/x86_64-linux-gnu/libIPSec_MB.so.0 /usr/lib/x86_64-linux-gnu/libIPSec_MB.so

# dpdk 19.11.14
cd ~
# wget https://fast.dpdk.org/rel/dpdk-19.11.14.tar.xz
# tar xf dpdk-19.11.14.tar.xz
# mv dpdk-stable-19.11.14 dpdk-inst
wget https://fast.dpdk.org/rel/dpdk-21.11.9.tar.xz
tar xf dpdk-21.11.9.tar.xz
mv dpdk-stable-21.11.9 dpdk-inst-21.11.9

cd ~/dpdk-inst-21.11.9
# Disable kernel modules to avoid KNI build issues on newer kernels
# rm -rf build && meson build -Denable_kmods=false
python3 -m venv ~/dpdk-venv
source ~/dpdk-venv/bin/activate
pip install --upgrade pip
pip install pyelftools meson

rm -rf build && meson build
cd ~/dpdk-inst-21.11.9/build
ninja
sudo ninja install
sudo ldconfig

# Decompress the DDP package, required for Intel ice driver in not safe mode (to create flow rules)
sudo zstd -d /lib/firmware/intel/ice/ddp/ice-1.3.36.0.pkg.zst -o /lib/firmware/intel/ice/ddp/ice.pkg