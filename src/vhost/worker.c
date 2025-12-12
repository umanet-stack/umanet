/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "src/config/config.h"
#include "src/eth/eth.h"
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
    rx_count = rte_eth_rx_burst(config.ports[0], vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
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
void drain_virtio_tx(struct vhost_dev *vdev) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    // copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
    count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, eth.mbuf_pool, pkts, MAX_PKT_BURST);

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

/*
 * Main function of vhost-switch. It basically does:
 *
 * for each vhost device {
 *    - drain_eth_rx()
 *
 *      Which drains the host eth Rx queue linked to the vhost device,
 *      and deliver all of them to guest virito Rx ring associated with
 *      this vhost device.
 *
 *    - drain_virtio_tx()
 *
 *      Which drains the guest virtio Tx queue and deliver all of them
 *      to the target, which could be another vhost device, or the
 *      physical eth dev. The route is done in function "virtio_tx_route".
 * }
 */
int switch_worker(void *arg __rte_unused) {
    unsigned i;
    unsigned lcore_id = rte_lcore_id();
    struct vhost_dev *vdev;
    struct mbuf_table *tx_q;

    RTE_LOG(INFO, VHOST_DATA, "Procesing on Core %u started\n", lcore_id);

    tx_q = &vhost.lcore_tx_queue[lcore_id];
    // Get pointer to this core's TX queue
    for (i = 0; i < rte_lcore_count(); i++) {
        if (vhost.lcore_ids[i] == lcore_id) {
            tx_q->txq_id = i;
            break;
        }
    }

    while (1) {
        drain_mbuf_table(tx_q); // drain if timeout has elapsed

        /*
         * Inform the configuration core that we have exited the
         * linked list and that no devices are in use if requested.
         */
        if (vhost.lcore_info[lcore_id].dev_removal_flag == REQUEST_DEV_REMOVAL)
            vhost.lcore_info[lcore_id].dev_removal_flag = ACK_DEV_REMOVAL;

        /*
         * Process vhost devices
         */
        TAILQ_FOREACH(vdev, &vhost.lcore_info[lcore_id].vdev_list, lcore_vdev_entry) {
            if (unlikely(vdev->remove)) { // device is marked for removal
                unlink_vmdq(vdev);
                vdev->ready = DEVICE_SAFE_REMOVE;
                continue;
            }

            if (likely(vdev->ready == DEVICE_RX))
                drain_eth_rx(vdev); // receive packets from physical NIC and forward them to a VM

            if (likely(!vdev->remove)) // device is not being removed (double-check)
                drain_virtio_tx(vdev); // receive packets from VM's TX queue, route them to the correct destination
        }
    }

    return 0;
}