# tap
## vm-vm-internal
For `vm-vm-internal`, if you run 8 vms = 4 servers + 4 clients = `report-4vm`
```bash
# need to rerun br/tap setup after dpdk test
./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-vm-internal

vcpus=$(ps -eLo pid,tid,comm | grep cloud-hyperviso | awk '{print $2}')
echo $vcpus

# sample traffic for 10s, -g = records call stacks
sudo perf record -p $(echo $vcpus | tr ' ' ',') -g -- sleep 10

sudo apt update
sudo apt install linux-tools-common linux-tools-$(uname -r)
# sudo perf report
# sudo perf report --sort overhead,symbol
# sudo perf report --sort cpu
# sudo perf report -g

# get sorted table of %cpu cycles
# About 52% of VM CPU cycles are spent in TAP/networking code -> justify 1 dedicated core = 2 shared cores
sudo perf report --no-children


sudo perf report -n --stdio | \
grep -E 'tun_|netif_|skb_|tcp_|udp_|_copy_' | \
awk '{sum += $2} END {print "Networking Self % =", sum}'


```
## multinode
```bash
# node 1
./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-server
# node 0
./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-client
python testing/process_logs/main.py tap vm-client

sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9"
```
