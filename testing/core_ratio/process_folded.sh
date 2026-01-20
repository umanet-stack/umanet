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
# /virtio_|vhost_|vring_|virtqueue/ {
#     virtio += $NF; next
# }
/netif_|skb_|tcp_|udp_|ip_rcv|napi_|net_rx|sock_|_copy_|gro_|gso_/ {
    net += $NF; next
}
/irq_|__irq|softirq|__softirq|tasklet|hrtimer/ {
    irq += $NF; next
}
/kvm_vcpu|vcpu_run/ {
    kvm += $NF; next
}
/schedule|__schedule|kvm_vcpu_block/ {
    sched += $NF; next
}
/spin_|mutex_|rwsem_|lock_|_unlock|contention/ {
    lock += $NF; next
}
/page_fault|handle_mm_fault|alloc_|free_|kmem_|slab_|mm_|__alloc_pages|copy_user/ {
    mm += $NF; next
}
/sys_|do_syscall|entry_SYSCALL|exit_to_user|__x64_sys/ {
    syscall += $NF; next
}
# /blk_|bio_|submit_bio|nvme_|scsi_/ {
#     block += $NF; next
# }
# /driver_|pci_|dma_|iommu_|mlx_|ixgbe_|ena_|e1000/ {
#     driver += $NF; next
# }
{
    other += $NF
}
END {
    total = tap + net + virtio + irq + kvm + sched + lock + mm + syscall + block + driver + other

    printf "=== Absolute samples ===\n"
    printf "TAP networking: %d\n", tap
    printf "Kernel networking (non-TAP): %d\n", net
    # printf "Virtio/Vhost: %d\n", virtio
    printf "Interrupts/Softirq: %d\n", irq
    printf "KVM: %d\n", kvm
    printf "Scheduler: %d\n", sched
    printf "Locks/Contention: %d\n", lock
    printf "Memory/MM: %d\n", mm
    printf "Syscalls: %d\n", syscall
    # printf "Block I/O: %d\n", block
    # printf "Drivers: %d\n", driver
    printf "Other: %d\n", other
    printf "Total: %d\n\n", total

    printf "=== Percent of total ===\n"
    printf "TAP networking: %.2f%%\n", 100*tap/total
    printf "Kernel networking (non-TAP): %.2f%%\n", 100*net/total
    # printf "Virtio/Vhost: %.2f%%\n", 100*virtio/total
    printf "Interrupts/Softirq: %.2f%%\n", 100*irq/total
    printf "KVM: %.2f%%\n", 100*kvm/total
    printf "Scheduler: %.2f%%\n", 100*sched/total
    printf "Locks/Contention: %.2f%%\n", 100*lock/total
    printf "Memory/MM: %.2f%%\n", 100*mm/total
    printf "Syscalls: %.2f%%\n", 100*syscall/total
    # printf "Block I/O: %.2f%%\n", 100*block/total
    # printf "Drivers: %.2f%%\n", 100*driver/total
    printf "Other: %.2f%%\n", 100*other/total
}' "$FOLDED_FILE"