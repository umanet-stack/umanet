# report packages
```bash
sudo apt update && sudo apt install -y python3-matplotlib python3-numpy 2>&1 | tail -15
```

# tap
For `samenode`, if you run 8 vms = 4 servers + 4 clients = `report-4vm`
```bash
# need to rerun br/tap setup after dpdk test
./setup/vm/setup_br_tap.sh 0 enp23s0f0np0 32
./setup/vm/spawn_vms.sh tap 32 /tmp samenode
python testing/process_results.py tap samenode

./setup/vm/spawn_vms.sh tap 32 /tmp multinode
python testing/process_results.py tap multinode

```

# dpdk
```bash
# run TAP once before DPDK to make it download iperf
# no. of vhost must match no. of VMs!
sudo ./build_and_run.sh 0 enp23s0f0np0 0000:17:00.0 test 5 32
./setup/vm/spawn_vms.sh dpdk 32 /tmp samenode
python testing/process_results.py dpdk samenode

sudo ./build_and_run.sh 0 enp23s0f0np0 0000:17:00.0 test 5 32
./setup/vm/spawn_vms.sh dpdk 32 /tmp multinode
python testing/process_results.py dpdk multinode

# kill all vms to end/reset experiment
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

## multinode setup
```bash
# make sure the set other node nic
sudo ip addr flush dev enp23s0f0np0
sudo ip addr add 192.168.100.99/24 dev enp23s0f0np0
sudo ip link set enp23s0f0np0 up
```

# OVS DPDK

Note that OVS DPDK requires DPDK version 24.11.3 so probably incompatible with other test.

```
command -v cloud-hypervisor || (curl -L https://github.com/cloud-hypervisor/cloud-hypervisor/releases/download/v50.0/cloud-hypervisor-static -o ch && sudo install ch -m 0755 /usr/bin/cloud-hypervisor)
[ -f /tmp/noble-server-cloudimg-amd64.raw -a -f /tmp/vmlinux.bin ] || ./setup/img/download_img.sh
sudo sysctl -w vm.nr_hugepages=24576
sudo ./setup/ovs/install.sh
sudo ./setup/ovs/setup.sh 32
sudo ./setup/img/build_ovs_image.sh /tmp/noble-server-cloudimg-amd64.raw
sudo ./setup/img/build_initramfs.sh
sudo ./setup/img/build_rw_disk.sh 32 512

sudo ./setup/vm/spawn_vms.sh ovs_dpdk 32 /tmp samenode
python testing/process_results.py ovs_dpdk samenode

sudo ./setup/vm/spawn_vms.sh ovs_dpdk 32 /tmp multinode
python testing/process_results.py ovs_dpdk multinode
```

## manual
```bash
# testing
iperf -s
iperf -c 192.168.100.2

iperf3 -s
iperf3 -c 192.168.100.2 -P 4 -t 10 
iperf3 -c 192.168.100.99 -P 4 -t 10
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
sudo tcpdump -i enp23s0f0np0
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

# no. of TX/RX queues in NIC e.g. combined 32 = 32TX + 32RX
# canonical: 1 core uses 1TX + 1RX
ethtool -l enp23s0f0np0
```