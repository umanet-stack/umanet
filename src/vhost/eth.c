#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/fast/network.h"
#include "src/include/fastpath.h"
#include "src/utils/utils.h"
#include "src/vhost/vhost.h"

// receive packets from physical NIC and forward them to a VM
void poll_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    // Validate device before attempting to use it
    if (unlikely(vdev == NULL)) {
        printf("Error: NULL vdev in poll_eth_rx\n");
        return;
    }

    // Check for obviously invalid vid (could indicate freed/corrupted memory)
    if (unlikely(vdev->vid < 0 || vdev->vid >= 64)) {
        printf("Error: Invalid vid=%d in poll_eth_rx (possible use-after-free)\n", vdev->vid);
        return;
    }

    if (unlikely(vdev->remove || vdev->ready != DEVICE_RX)) {
        printf("Warning: Attempting to poll RX for device vid=%d not in RX state (ready=%d, remove=%d)\n", vdev->vid,
               vdev->ready, vdev->remove);
        return;
    }

    // Validate vmdq_rx_q is in reasonable range
    if (unlikely(vdev->rx_queue >= 256)) {
        printf("Error: Invalid vmdq_rx_q=%d for vid=%d\n", vdev->rx_queue, vdev->vid);
        return;
    }

    // receive packets from physical NIC
    rx_count = rte_eth_rx_burst(net_port_id, vdev->rx_queue, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;
    // printf("Received %d packets from physical NIC\n", rx_count);

    // send packets to guest virtio RX ring
    enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);

    // Check if enqueue failed completely (could indicate disconnected device)
    if (unlikely(enqueue_count == 0 && rx_count > 0)) {
        printf("Warning: Failed to enqueue any packets to vid=%d (may be disconnected)\n", vdev->vid);
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
        printf("Error: NULL tx_q in flush_eth_tx\n");
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

    log_pkt_out("(%d) Flushing %d packets to NIC", tx_q->txq_id, tx_q->len);
    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}