#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/fast/nat.h"
#include "src/fast/network.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"

// receive packets from physical NIC and forward them to a VM
void poll_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    if (unlikely(check_device_state(vdev, "poll_eth_rx") != 0))
        return;

    rx_count = rte_eth_rx_burst(net_port_id, vdev->rx_queue, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;
    LOG_PKT_IN("Received %d packets from physical NIC\n", rx_count);
    PRINT_PKTS(pkts, rx_count, LOG_PKT_IN);

    // Apply reverse NAT to determine destination VM
    uint16_t nat_count = 0;
    for (uint16_t i = 0; i < rx_count; i++) {
        int target_vid = -1;
        if (nat_translate_inbound(pkts[i], &target_vid) == 0) {
            // NAT translation successful, check if it's for this vdev
            if (target_vid == vdev->vid) {
                pkts[nat_count++] = pkts[i];
            } else {
                // Packet is for a different VM, need to forward it there
                // For now, just drop it (TODO: implement cross-vdev forwarding)
                LOG_WARN("Packet is for vid=%d but received on vid=%d, dropping\n", target_vid, vdev->vid);
                rte_pktmbuf_free(pkts[i]);
            }
        } else {
            // Not a NAT'd packet, forward normally
            pkts[nat_count++] = pkts[i];
        }
    }

    if (nat_count == 0)
        return;

    enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, nat_count);
    LOG_PKT_OUT("Enqueued %d packets to guest virtio RX ring\n", enqueue_count);
    PRINT_PKTS(pkts, enqueue_count, LOG_PKT_OUT);

    if (unlikely(enqueue_count == 0 && nat_count > 0)) {
        LOG_WARN("Warning: Failed to enqueue any packets to vid=%d (may be disconnected)\n", vdev->vid);
        free_pkts(pkts, nat_count);
        vdev->remove = 1; // Mark device for removal
        return;
    }

    /* Retry if necessary */
    if (config.enable_retry && unlikely(enqueue_count < nat_count)) {
        uint32_t retry = 0;

        while (enqueue_count < nat_count && retry++ < config.burst_rx_retry_num) { // max 4 retries
            rte_delay_us(config.burst_rx_delay_time);
            enqueue_count +=
                rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &pkts[enqueue_count], nat_count - enqueue_count);
        }
    }

    if (config.enable_stats) {
        rte_atomic64_add(&vdev->stats.rx_total_atomic, nat_count);
        rte_atomic64_add(&vdev->stats.rx_atomic, enqueue_count);
    }

    free_pkts(pkts, nat_count);
}

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
void flush_eth_tx(struct mbuf_table *tx_q) {
    uint16_t count;
    if (unlikely(tx_q == NULL)) {
        LOG_ERROR("Error: NULL tx_q in flush_eth_tx\n");
        return;
    }

    if (unlikely(tx_q->len == 0))
        return;

    // Gateway mode: Change MACs for proper routing
    // VMs send to gateway MAC 02:00:00:00:00:fe, we forward to physical gateway
    struct rte_ether_hdr *eth_hdr;
    struct rte_ether_addr gateway_mac = {{0x18, 0x5a, 0x58, 0x34, 0x49, 0xe4}}; // Physical gateway MAC
    for (int i = 0; i < tx_q->len; i++) {
        eth_hdr = rte_pktmbuf_mtod(tx_q->m_table[i], struct rte_ether_hdr *);
        rte_ether_addr_copy(&eth_addr, &eth_hdr->s_addr);    // Src: NIC's MAC
        rte_ether_addr_copy(&gateway_mac, &eth_hdr->d_addr); // Dst: Gateway's MAC
    }

    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    LOG_PKT_OUT("(%d) Sent %d packets to NIC\n", tx_q->txq_id, count);
    PRINT_PKTS(tx_q->m_table, count, LOG_PKT_OUT);

    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}