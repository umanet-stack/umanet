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
	--kernel /proj/faasnetworkstack-PG0/testing/kernels/vm0-kernel.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/proj/faasnetworkstack-PG0/testing/images/vm0-img.raw path=/tmp/cloudinit/cloudinit-vm0.img \
	--net "tap=tap0,mac=12:34:56:78:90:00" 

# vm1
sudo cloud-hypervisor \
	--cpus boot=1 \
	--memory size=512M \
	--kernel /proj/faasnetworkstack-PG0/testing/kernels/vm1-kernel.bin \
	--cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
	--disk path=/proj/faasnetworkstack-PG0/testing/images/vm1-img.raw path=/tmp/cloudinit/cloudinit-vm1.img \
	--net "tap=tap1,mac=12:34:56:78:90:01" 

# testing
iperf -s
iperf -c 192.168.100.2

iperf3 -s
iperf3 -c 192.168.100.2 -P 4 -t 10 -J
# uses 4 cores, 11Gi

iperf3 -c 192.168.100.2 -P 4 -t 10 -J \
| jq --arg vm "vm7" '. + {vm: $vm}' \
| nc -N 192.168.100.1 9000

# vCPU usage
mpstat -P ALL 1
# memory usage
free -h

systemctl status iperf
```