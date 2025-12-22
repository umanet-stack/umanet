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
./setup/tap/spawn_vms.sh 16 /proj/faasnetworkstack-PG0/testing

# run TAP one before DPDK to make it download iperf
### DPDK #######
# make sure the set other node nic
sudo ip addr flush dev enp65s0f0np0
sudo ip addr add 192.168.100.99/24 dev enp65s0f0np0
sudo ip link set enp65s0f0np0 up

# SUPER IMPORTANT: no. of vhost must match no. of VMs!
sudo ./build_and_run.sh 0000:41:00.0 test 3 24
./setup/dpdk/spawn_vms.sh 24 /proj/faasnetworkstack-PG0/testing

sudo ./build_and_run.sh 0000:41:00.0 test 3 16
./setup/dpdk/spawn_vms.sh 16 /proj/faasnetworkstack-PG0/testing

sudo ./build_and_run.sh 0000:41:00.0 test 3 8
./setup/dpdk/spawn_vms.sh 8 /proj/faasnetworkstack-PG0/testing
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
# Poll cores → ~100% usr
# High %softirq → kernel networking leaking in
# High %steal → oversubscribed host / VM
# only add dpdk cores when VM vCPU < 70%
# enqueue fails = vms can't clear rx rings fast enough
mpstat -P ALL 1

# memory usage
free -h
# check space
df -h

systemctl status iperf
sudo tcpdump -i br0
echo '{"vm":"vm12","throughput":12345}' | nc 192.168.100.1 9000

lsof -i :9000
kill -9 12345
```

### Profiling
PID        Process ID
USER       Owner
PR / NI    Priority / nice
VIRT       Virtual address space
RES        Resident memory (actual RAM used)
SHR        Shared memory
S          State (R=running, S=sleeping, I=idle)
%CPU       CPU usage (can exceed 100%)
TIME+      Total CPU time used
COMMAND    Process name
```bash
# process info
top

# which threads burn cpu most
# decides if you need more dpdk fp cores
top -H

# allow perf to profile all processes
sudo sysctl -w kernel.perf_event_paranoid=0
# which functions burn cpu most
perf top
# hot functions per thread
perf top -H
```