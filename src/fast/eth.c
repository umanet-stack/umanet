#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

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

    enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);
    LOG_PKT_OUT("Enqueued %d packets to guest virtio RX ring\n", enqueue_count);
    PRINT_PKTS(pkts, enqueue_count, LOG_PKT_OUT);

    if (unlikely(enqueue_count == 0 && rx_count > 0)) {
        LOG_WARN("Warning: Failed to enqueue any packets to vid=%d (may be disconnected)\n", vdev->vid);
        free_pkts(pkts, rx_count);
        vdev->remove = 1; // Mark device for removal
        return;
    }

    /* Retry if necessary */
    if (config.enable_retry && unlikely(enqueue_count < rx_count)) {
        uint32_t retry = 0;

        while (enqueue_count < rx_count && retry++ < config.burst_rx_retry_num) { // max 4 retries
            rte_delay_us(config.burst_rx_delay_time);
            enqueue_count +=
                rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &pkts[enqueue_count], rx_count - enqueue_count);
        }
    }

    if (config.enable_stats) {
        rte_atomic64_add(&vdev->stats.rx_total_atomic, rx_count);
        rte_atomic64_add(&vdev->stats.rx_atomic, enqueue_count);
    }

    free_pkts(pkts, rx_count);
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

    // Set source MAC to NIC port MAC
    struct rte_ether_hdr *eth_hdr;
    for (int i = 0; i < tx_q->len; i++) {
        eth_hdr = rte_pktmbuf_mtod(tx_q->m_table[i], struct rte_ether_hdr *);
        rte_ether_addr_copy(&eth_addr, &eth_hdr->s_addr);
    }

    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    LOG_PKT_OUT("(%d) Sent %d packets to NIC\n", tx_q->txq_id, count);
    PRINT_PKTS(tx_q->m_table, count, LOG_PKT_OUT);

    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}