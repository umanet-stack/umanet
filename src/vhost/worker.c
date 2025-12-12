/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "src/config/config.h"
#include "src/eth/eth.h"
#include "src/fast/network.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"

#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>

// receive packets from physical NIC and forward them to a VM
void drain_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    // receive packets from physical NIC
    rx_count = rte_eth_rx_burst(net_port_id, vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;

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

// receive packets from VM's TX queue, route them to the correct destination
void drain_virtio_tx(struct vhost_dev *vdev, struct dataplane_context *ctx) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    // copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
    count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, ctx->net.pool, pkts, MAX_PKT_BURST);

    /* setup VMDq for the first packet */
    if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) { // device in MAC learning
        if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1)      // failed to learn MAC from first packet
            free_pkts(pkts, count);
    }

    for (i = 0; i < count; ++i) {                                 // loop received packets
        virtio_tx_route(vdev, pkts[i], eth.vlan_tags[vdev->vid]); // route each to correct destination
    }
}

void drain_mbuf_table(struct mbuf_table *tx_q) {
    // static = function-scope, keeps value between function calls
    static uint64_t prev_tsc; // previous timestamp
    uint64_t cur_tsc;

    if (tx_q->len == 0)
        return;

    cur_tsc = rte_rdtsc(); // current timestamp
    if (unlikely(cur_tsc - prev_tsc > MBUF_TABLE_DRAIN_TSC)) {
        // time elapsed since last drain exceeds threshold
        prev_tsc = cur_tsc;

        RTE_LOG_DP(DEBUG, VHOST_DATA, "TX queue drained after timeout with burst size %u\n", tx_q->len);
        do_drain_mbuf_table(tx_q);
    }
}
