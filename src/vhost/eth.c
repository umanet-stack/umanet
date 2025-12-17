#include "src/fast/network.h"
#include "src/include/fastpath.h"
#include "src/utils/utils.h"
#include "src/vhost/vhost.h"

#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

// receive packets from physical NIC and forward them to a VM
void poll_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    // receive packets from physical NIC
    rx_count = rte_eth_rx_burst(net_port_id, vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;
    // printf("Received %d packets from physical NIC\n", rx_count);

    // send packets to guest virtio RX ring
    enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);

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

    printf("do_drain_mbuf_table\n");
    printf("txq_id: %d\n", tx_q->txq_id);
    printf("len: %d\n", tx_q->len);
    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    printf("count: %d\n", count);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}