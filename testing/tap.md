# tap
```bash
# c6525-25g
./setup/setup_node.sh 0 enp65s0f0np0

# xl170
./setup/setup_node.sh 0 ens1f1np1

./setup/vanilla/setup_br_tap.sh 0

# vm0
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vm0-kernel.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/tmp/vm0-img.raw path=/tmp/cloudinit-vm0.img \
	--net "tap=tap0,mac=52:54:00:02:d9:01" 

sudo ip link set ens4 up
sudo ip addr add 192.168.100.2/24 dev ens4
sudo ip route add default via 192.168.100.1

# vm1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /tmp/vm1-kernel.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/tmp/vm1-img.raw path=/tmp/cloudinit-vm1.img \
	--net "tap=tap1,mac=52:54:20:11:C5:02" 

sudo ip link set ens4 up
sudo ip addr add 192.168.100.3/24 dev ens4
sudo ip route add default via 192.168.100.1

# testing
iperf -s
iperf -c 192.168.100.2
# uses 4 cores, 11Gi

# vCPU usage
mpstat -P ALL 1
# memory usage
free -h
```