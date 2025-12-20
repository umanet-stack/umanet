# tap
```bash
./setup/download_img.sh
# Cloud-init will NOT run again on these images, it only runs on first boot.
# if you modify anything in cloud-init, you need to run copy_img and gen-cloud-init again.
# copies 1 img/kernel per vm to /proj/{your_cloudlab_project}/testing
./setup/copy_img.sh 24 /proj/faasnetworkstack-PG0/testing
# ./setup/copy_img.sh 24 /tmp

./setup/cloudinit/gen-cloud-init.sh

# c6525-25g
./setup/setup_node.sh 0 enp65s0f0np0
# xl170
./setup/setup_node.sh 0 ens1f1np1

python ./testing/collector.py

# disable SMT (2 threads/core => 1 thread/core)
echo off | sudo tee /sys/devices/system/cpu/smt/control

### TAP ########
./setup/tap/setup_br_tap.sh 0
./setup/tap/spawn_vms.sh 24 /proj/faasnetworkstack-PG0/testing

# run TAP one before DPDK to make it download iperf
### DPDK #######
./setup/dpdk/setup_vtap.sh
sudo ./build_and_run.sh 0000:41:00.0 test 3 32
./setup/dpdk/spawn_vms.sh 24 /proj/faasnetworkstack-PG0/testing
################

# process results
sudo apt update && sudo apt install -y python3-matplotlib python3-numpy 2>&1 | tail -15
python testing/process_results.py

# kill all vms to end/reset experiment
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

## manual
```bash
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
# check space
df -h

systemctl status iperf
```