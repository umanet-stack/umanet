/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "src/config/config.h"
#include "src/eth/eth.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"

#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>

// receive packets from physical NIC and forward them to a VM
static __rte_always_inline void drain_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    // receive packets from physical NIC
    rx_count = rte_eth_rx_burst(config.ports[0], vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;

    if (config.builtin_net_driver) {
        // send packets to guest virtio RX ring
        enqueue_count = vs_enqueue_pkts(vdev, VIRTIO_RXQ, pkts, rx_count);
    } else {
        enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);
    }

    /* Retry if necessary */
    if (config.enable_retry && unlikely(enqueue_count < rx_count)) {
        uint32_t retry = 0;

        while (enqueue_count < rx_count && retry++ < config.burst_rx_retry_num) { // max 4 retries
            rte_delay_us(config.burst_rx_delay_time);
            if (config.builtin_net_driver) {
                enqueue_count += vs_enqueue_pkts(vdev, VIRTIO_RXQ, &pkts[enqueue_count], rx_count - enqueue_count);
            } else {
                enqueue_count +=
                    rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &pkts[enqueue_count], rx_count - enqueue_count);
            }
        }
    }

    if (config.enable_stats) {
        rte_atomic64_add(&vdev->stats.rx_total_atomic, rx_count);
        rte_atomic64_add(&vdev->stats.rx_atomic, enqueue_count);
    }

    free_pkts(pkts, rx_count);
}

// receive packets from VM's TX queue, route them to the correct destination
static __rte_always_inline void drain_virtio_tx(struct vhost_dev *vdev) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    if (config.builtin_net_driver) {
        // copy pkt from guest vring buffer to DPDK mbuf
        count = vs_dequeue_pkts(vdev, VIRTIO_TXQ, eth.mbuf_pool, pkts, MAX_PKT_BURST);
    } else {
        count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, eth.mbuf_pool, pkts, MAX_PKT_BURST);
    }

    /* setup VMDq for the first packet */
    if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) { // device in MAC learning
        if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1)      // failed to learn MAC from first packet
            free_pkts(pkts, count);
    }

    for (i = 0; i < count; ++i) { // loop received packets
        // NEW: Try TCP classification
        // uint32_t sip, dip;
        // uint16_t sport, dport;
        // struct tcp_flow_state *flow = NULL;

        // if (vdev->tcp_offload_enabled && tcp_parse_packet(pkts[i], &sip, &dip, &sport, &dport) == 0) {
        //     // Lookup existing flow
        //     flow = tcp_flow_lookup(sip, dip, sport, dport);

        //     if (!flow) {
        //         // Check if SYN packet
        //         struct rte_tcp_hdr *tcp = rte_pktmbuf_mtod(pkts[i], struct rte_tcp_hdr *);
        //         if (tcp->tcp_flags & RTE_TCP_SYN_FLAG) {
        //             flow = tcp_flow_create(sip, dip, sport, dport, vdev->vid);
        //         }
        //     }
        //     if (flow) {
        //         RTE_LOG_DP(DEBUG, VHOST_DATA, "Packet belongs to tracked flow\n");
        //         // tcp_flow_update(flow, pkts[i], 1 /* inbound */);
        //         // Still forward via L2 for now
        //     }
        // }
        virtio_tx_route(vdev, pkts[i], eth.vlan_tags[vdev->vid]); // route each to correct destination
    }
}

static __rte_always_inline void drain_mbuf_table(struct mbuf_table *tx_q) {
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
    uint16_t id = (uintptr_t)arg;

    struct dataplane_context *ctx;

    /* Allocate fastpath core context */
    // if ((ctx = rte_zmalloc("fastpath core context", sizeof(*ctx), 0)) == NULL) {
    //     fprintf(stderr, "Allocating fastpath core context failed\n");
    //     goto error_alloc;
    // }
    // ctxs[id] = ctx;
    // ctx->id = id;

    /* initialize data plane context */
    // if (dataplane_context_init(ctx) != 0) {
    //     fprintf(stderr, "initializing data plane context\n");
    //     goto error_dpctx;
    // }

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

error_dpctx:
error_alloc:
    return -1;
}