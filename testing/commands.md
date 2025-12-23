# tap
```bash
./setup/tap/setup_br_tap.sh 0
./setup/tap/spawn_vms.sh 2 /tmp samenode
./setup/tap/spawn_vms.sh 16 /tmp multinode

# process results
sudo apt update && sudo apt install -y python3-matplotlib python3-numpy 2>&1 | tail -15
python testing/process_results.py tap

# kill all vms to end/reset experiment
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

# dpdk
```bash
# run TAP once before DPDK to make it download iperf
# make sure the set other node nic
sudo ip addr flush dev enp65s0f0np0
sudo ip addr add 192.168.100.99/24 dev enp65s0f0np0
sudo ip link set enp65s0f0np0 up

# no. of vhost must match no. of VMs!
sudo ./build_and_run.sh 0000:41:00.0 test 3 32
./setup/dpdk/spawn_vms.sh 32 /proj/faasnetworkstack-PG0/testing

sudo ./build_and_run.sh 0000:41:00.0 test 3 16
./setup/dpdk/spawn_vms.sh 16 /proj/faasnetworkstack-PG0/testing

sudo ./build_and_run.sh 0000:41:00.0 test 3 8
./setup/dpdk/spawn_vms.sh 8 /proj/faasnetworkstack-PG0/testing

# process results
python testing/process_results.py dpdk

# kill all vms to end/reset experiment
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
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
sudo tcpdump -i enp65s0f0np0
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
ethtool -l enp65s0f0np0
```