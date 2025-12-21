/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef FASTPATH_H_
#define FASTPATH_H_

#include <rte_ether.h>
#include <stdbool.h>
#include <stdint.h>

#include <rte_interrupts.h>

#include "../../include/tas_memif.h"
#include "../../include/utils_rng.h"

#define BATCH_SIZE 16
#define BUFCACHE_SIZE 128
#define TXBUF_SIZE (2 * BATCH_SIZE)

#define DATAPLANE_TSCS

#ifdef DATAPLANE_STATS
#ifdef DATAPLANE_TSCS
#define STATS_TS(n) uint64_t n = rte_get_tsc_cycles()
#define STATS_TSADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#endif
#define STATS_ADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#define STATS_ADD(c, f, n)                                                                                             \
    do {                                                                                                               \
    } while (0)
#endif

struct network_thread {
    struct rte_mempool *pool;
    uint16_t queue_id;
};

/** Skiplist: #levels */
#define QMAN_SKIPLIST_LEVELS 4

struct qman_thread {
    /************************************/
    /* read-only */
    struct queue *queues;

    /************************************/
    /* modified by owner thread */
    uint32_t head_idx[QMAN_SKIPLIST_LEVELS];
    uint32_t nolimit_head_idx;
    uint32_t nolimit_tail_idx;
    uint32_t ts_real;
    uint32_t ts_virtual;
    struct utils_rng rng;
    bool nolimit_first;
};

struct device_statistics {
    uint64_t tx;
    uint64_t tx_total;
    rte_atomic64_t rx_atomic;
    rte_atomic64_t rx_total_atomic;
};

struct vhost_dev { // vhost device
    // Device MAC address (Obtained on first TX packet).
    struct rte_ether_addr mac_address;
    // ETH RX queue number assigned to vhost device
    uint16_t rx_queue;
    /**< Data core that the device is added to. */
    uint16_t coreid;
    /**< A device is set as ready if the MAC address has been set. */
    volatile uint8_t ready;
    /**< Device is marked for removal from the data core. */
    volatile uint8_t remove;

    int vid;                      // vhost device ID, assigned by dpdk
    uint64_t features;            // Virtio feature flags
    size_t hdr_len;               // Header length
    uint16_t nr_vrings;           // Number of virtio rings
    struct rte_vhost_memory *mem; // Guest memory mapping
    struct device_statistics stats;

    // Rate-limited logging for failed enqueue attempts
    uint64_t last_failed_log_ts; // TSC timestamp of last log
    uint64_t failed_pkts_count;  // Cumulative failed packets since last log
} __rte_cache_aligned;

#define MAX_PKT_BURST 32              /* Max packets processed per burst (RX/TX) */
#define MAX_VHOST_DEVICES_PER_CORE 64 /* Max vhost devices per dataplane core */

/* Used for queueing bursts of TX packets. */
struct mbuf_table {
    unsigned len;
    unsigned txq_id;
    struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct vhost_info {
    uint32_t device_num;

    /* Flag to synchronize device removal. */
    volatile uint8_t dev_removal_flag;

    // Array of device pointers for round-robin polling
    struct vhost_dev *vdev_list[MAX_VHOST_DEVICES_PER_CORE];

    // Round-robin index for polling devices
    uint32_t poll_next_device;

    struct mbuf_table tx_q;
};

struct dataplane_context {
    struct network_thread net;
    struct qman_thread qman;
    struct rte_ring *qman_fwd_ring;
    uint16_t id;
    int evfd;
    struct rte_epoll_event ev;

    // vhost
    struct vhost_info vhost;

    /********************************************************/
    /* send buffer */
    struct network_buf_handle *tx_handles[TXBUF_SIZE];
    uint16_t tx_num;

    /********************************************************/
    /* polling queues */
    uint32_t poll_next_ctx;

    /********************************************************/
    /* pre-allocated buffers for polling doorbells and queue manager */
    struct network_buf_handle *bufcache_handles[BUFCACHE_SIZE];
    uint16_t bufcache_num;
    uint16_t bufcache_head;

    uint64_t loadmon_cyc_busy;

    uint64_t kernel_drop;
    /********************************************************/
    /* Stats */
    // uint64_t stat_qm_poll;
    // uint64_t stat_qm_empty;
    // uint64_t stat_qm_total;

    // uint64_t stat_rx_poll;
    // uint64_t stat_rx_empty;
    // uint64_t stat_rx_total;

    // uint64_t stat_qs_poll;
    // uint64_t stat_qs_empty;
    // uint64_t stat_qs_total;
    uint64_t stat_pkt_vhost_rx;
    uint64_t stat_pkt_vhost_tx;
    uint64_t stat_pkt_vhost_tx_fail;

    uint64_t stat_cyc_loop;
    uint64_t stat_cyc_loop_sleep;
    uint64_t stat_cyc_loop_vdev;
    uint64_t stat_cyc_loop_vhost;

    uint64_t stat_cyc_poll_eth;
    uint64_t stat_cyc_flush_eth;
    uint64_t stat_cyc_poll_vhost;
    uint64_t stat_cyc_route_vhost;
    uint64_t stat_cyc_virtio_tx;
};

extern struct dataplane_context **ctxs;

int dataplane_init(void);
int dataplane_context_init(struct dataplane_context *ctx);
void dataplane_context_destroy(struct dataplane_context *ctx);
void dataplane_loop(struct dataplane_context *ctx);
void dataplane_dump_stats(void);

#endif /* ndef FASTPATH_H_ */
