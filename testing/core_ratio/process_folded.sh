#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: $0 <stacks.folded>"
    exit 1
fi

FOLDED_FILE=$1

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
}' $FOLDED_FILE