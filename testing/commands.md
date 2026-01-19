# report packages
```bash
sudo apt update && sudo apt install -y python3-matplotlib python3-numpy 2>&1 | tail -15
```

The tests (iperf, sockperf) are configured in `.env` file.
# tap
## vm-vm-internal
For `vm-vm-internal`, if you run 8 vms = 4 servers + 4 clients = `report-4vm`
```bash
./setup/cpu/slice_cpu.sh tap
# need to rerun br/tap setup after dpdk test
./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-vm-internal
python testing/process_logs/main.py tap vm-vm-internal

```
## multinode
```bash
# both nodes
./setup/cpu/slice_cpu.sh tap
./setup/vm/setup_br_tap.sh 32
# node 1
./setup/vm/spawn_vms.sh tap 32 vm-server
# node 0
./setup/vm/spawn_vms.sh tap 32 vm-client
python testing/process_logs/main.py tap vm-client
```

# dpdk
- vm user-data has ping service that will ping 3 times to make dpdk app learn IP of vm
## vm-vm-internal
```bash
./setup/cpu/slice_cpu.sh dpdk
# run TAP once before DPDK to make it download iperf
# no. of vhost must match no. of VMs!
sudo ./run.sh 32 1

# do local networking via dpdk
./setup/vm/spawn_vms.sh dpdk 32 vm-vm-internal
# do local networking via tap
./setup/vm/spawn_vms.sh dpdk-tap 32 vm-vm-internal
python testing/process_logs/main.py dpdk vm-vm-internal
ethtool -k ens6
```
## multinode
```bash
# both nodes (make sure to build as test mode first)
./setup/cpu/slice_cpu.sh dpdk
sudo ./run.sh 32 1
# node 1
./setup/vm/spawn_vms.sh dpdk 32 vm-server
# node 0
./setup/vm/spawn_vms.sh dpdk 32 vm-client
python testing/process_logs/main.py dpdk vm-client
```

```bash
# kill all vms to end/reset experiment
sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
sudo bash -c "ps aux | grep umanet | grep -v grep | awk '{print \$2}' | xargs kill -9"
```

# OVS DPDK
## Setup
```bash
# see setup_vm.md for vm image setup, now same image as TAP/DPDK
command -v cloud-hypervisor || (curl -L https://github.com/cloud-hypervisor/cloud-hypervisor/releases/download/v50.0/cloud-hypervisor-static -o ch && sudo install ch -m 0755 /usr/bin/cloud-hypervisor)
[ -f /tmp/noble-server-cloudimg-amd64.raw -a -f /tmp/vmlinux.bin ] || ./setup/img/download_img.sh
sudo sysctl -w vm.nr_hugepages=24576
sudo ./setup/ovs/install.sh
sudo ./setup/ovs/setup_service.sh
```
## multinode
```bash
# both nodes
./setup/cpu/slice_cpu.sh dpdk
# no. must match no. of vms (ovs is not smart enough to not poll from unattached vdevs)
sudo ./setup/ovs/setup_interfaces.sh 32 1
# node 1
./setup/vm/spawn_vms.sh ovs-dpdk 32 vm-server
# node 0
./setup/vm/spawn_vms.sh ovs-dpdk 32 vm-client
python testing/process_logs/main.py ovs-dpdk vm-client

# exit ovs
sudo systemctl stop ovs-dpdk
# show interfaces
sudo ovs-vsctl show

sudo ovs-appctl dpif-netdev/pmd-stats-show
sudo ovs-appctl dpif-netdev/pmd-rxq-show
sudo ovs-appctl dpctl/dump-flows | grep ip
sudo ovs-vsctl list Interface vhost-user0

# ovs-dpdk uses 1500 MTU, so need to set it, else 0 throughput
sudo ip link set ens5 mtu 9000 && iperf3 -s
sudo ip link set ens5 mtu 9000 && iperf3 -c 192.168.100.2 -P 4 -t 10
sudo ip link set ens5 mtu 1500 && iperf -s
sudo ip link set ens5 mtu 1500 && iperf -c 192.168.100.2 -P 8 -t 10 -w 8M

# large vms
sudo ./setup/ovs/setup_interfaces.sh 1 4

```

## Overall Report
```bash
./testing/plot_reports/main.py iperf
./testing/plot_reports/main.py iperf-udp
./testing/plot_reports/main.py sockperf
```

## manual
```bash
# jumbo
sudo ip link set eth0 mtu 1500
ethtool -k tap0
sudo ip link set dev enp23s0f0np0 mtu 9000
sudo ip link set dev ens6 mtu 9000
sudo ip link set dev ens6 mtu 1500
# work even with mtu 1500
ping -M do -s 8972 192.168.100.3
iperf3 -c 192.168.100.2 -M 8972

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

# server
sockperf server -i 192.168.100.2
# client
sockperf ping-pong -i 192.168.100.2 -m 64 -t 10

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
sudo tcpdump -vv -i eth1
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

htop

# no. of TX/RX queues in NIC e.g. combined 32 = 32TX + 32RX
# canonical: 1 core uses 1TX + 1RX
ethtool -l enp23s0f0np0
```