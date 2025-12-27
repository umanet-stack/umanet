sudo apt update
sudo apt install -y \
  pkg-config \
  build-essential \
  libnuma-dev \
  python3-pyelftools \
  libelf-dev \
  libarchive-dev \
  ninja-build \
  meson \
  libssl-dev libcap-ng-dev libunbound-dev \
  linux-headers-$(uname -r) \
  autoconf automake libtool \
  wget curl netcat-traditional graphviz python3-pyftpdlib \
  net-tools sparse flake8 rdma-core ibverbs-providers libibverbs-dev libpcap-dev \
  libsystemd-dev \
  qemu-kvm libvirt-daemon-system libvirt-clients \
  virtinst bridge-utils dh-python debhelper sphinx-common \
  fakeroot dh-autoreconf python3-pip libguestfs-tools \
  flex bison

sudo chown $(id -u):$(id -g) /usr/src

cd /usr/src/
wget https://fast.dpdk.org/rel/dpdk-25.11.tar.xz
tar xf dpdk-25.11.tar.xz
export DPDK_DIR=/usr/src/dpdk-25.11
cd $DPDK_DIR
export DPDK_BUILD=$DPDK_DIR/build
meson build
ninja -C build
sudo ninja -C build install
sudo ldconfig

cd /usr/src
git clone https://github.com/openvswitch/ovs.git --filter=blob:none --depth=1 --branch v3.6.1
cd ovs
./boot.sh
./configure --with-dpdk=shared CFLAGS="-Ofast -msse4.2 -mpopcnt" # TODO: static
make -j$(nproc)
sudo make install

sudo tee /etc/systemd/system/openvswitch.service >/dev/null << 'EOF'
[Unit]
Description=Open vSwitch (from source)
After=network.target
Wants=network.target

[Service]
Type=forking
ExecStart=/usr/local/share/openvswitch/scripts/ovs-ctl start --system-id=random
ExecStop=/usr/local/share/openvswitch/scripts/ovs-ctl stop
ExecReload=/usr/local/share/openvswitch/scripts/ovs-ctl restart
PIDFile=/usr/local/var/run/openvswitch/ovs-vswitchd.pid
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now openvswitch

cd /usr/src
curl -O https://downloads.es.net/pub/iperf/iperf-3.19.tar.gz
tar zxvf iperf-3.19.tar.gz
cd iperf-3.19
./configure --enable-static --disable-shared
make -j$(nproc)
sudo make install
