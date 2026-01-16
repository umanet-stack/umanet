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
sudo perf record -p $(echo $vcpus | tr ' ' ',') -g -- sleep 20

sudo apt update
sudo apt install linux-tools-common linux-tools-$(uname -r)
# sudo perf report
# sudo perf report --sort overhead,symbol
# sudo perf report --sort cpu
# sudo perf report -g

# get sorted table of %cpu cycles
# About 52% of VM CPU cycles are spent in TAP/networking code -> justify 1 dedicated core = 2 shared cores
sudo perf report --no-children
sudo perf report --children


sudo perf report -n --stdio | \
grep -E 'tun_|netif_|skb_|tcp_|udp_|_copy_' | \
awk '{sum += $2} END {print "Networking Self % =", sum}'

# perf script reads perf.data from wdir, converts perf’s binary recording into human-readable text
sudo perf script > stacks.raw

# don't forget to chmod +x
sudo ./testing/core_ratio/stackcollapse-perf.pl stacks.raw > stacks.folded
sudo ./testing/core_ratio/flamegraph.pl stacks.folded > stacks.svg

awk '
/tun_get_user|tun_chr_write_iter|tun_put_user|tun_rx/ {tap += $NF; next}
/kvm_vcpu|vcpu_run/ {kvm += $NF; next}
/schedule|__schedule|kvm_vcpu_block/ {sched += $NF; next}
{other += $NF}
END {
  total = tap + kvm + sched + other
  printf "TAP: %.2f%%\n", 100*tap/total
  printf "KVM: %.2f%%\n", 100*kvm/total
  printf "Sched: %.2f%%\n", 100*sched/total
  printf "Other: %.2f%%\n", 100*other/total
}' stacks.folded


awk '
/tun_get_user|tun_chr_write_iter|tun_put_user|tun_rx/ {
    tap += $NF; next
}
/kvm_vcpu|vcpu_run/ {
    kvm += $NF; next
}
/schedule|__schedule|kvm_vcpu_block/ {
    sched += $NF; next
}
/netif_|skb_|tcp_|udp_|ip_rcv|napi_|net_rx|sock_|_copy_|gro_|gso_/ {
    net += $NF; next
}
{
    other += $NF
}
END {
    total = tap + kvm + sched + net + other

    printf "=== Absolute samples ===\n"
    printf "TAP networking: %d\n", tap
    printf "Kernel networking (non-TAP): %d\n", net
    printf "KVM: %d\n", kvm
    printf "Scheduler: %d\n", sched
    printf "Other: %d\n", other
    printf "Total: %d\n\n", total

    printf "=== Percent of total ===\n"
    printf "TAP networking: %.2f%%\n", 100*tap/total
    printf "Kernel networking (non-TAP): %.2f%%\n", 100*net/total
    printf "KVM: %.2f%%\n", 100*kvm/total
    printf "Scheduler: %.2f%%\n", 100*sched/total
    printf "Other: %.2f%%\n", 100*other/total
}' stacks-tap.folded


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
