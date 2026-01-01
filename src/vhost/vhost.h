/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef VHOST_H_
#define VHOST_H_

#include "log.h"
#include <rte_errno.h>
#include <rte_ether.h>
#include <rte_vhost.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/queue.h>

enum { VIRTIO_RXQ, VIRTIO_TXQ };

#define REQUEST_DEV_REMOVAL 1
#define ACK_DEV_REMOVAL 0

#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MBUF_TABLE_DRAIN_TSC ((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US)

extern const struct rte_vhost_device_ops virtio_net_device_ops;
extern struct route_table route_table;

// SP, mutated freely, not cached aligned
struct vhost_ctrl {
    int vid;
    uint64_t features;
    size_t hdr_len;
    struct rte_vhost_memory *mem;

    struct rte_ether_addr mac;
    uint32_t ip;

    // lifecycle
    bool attached;
};

struct vhost_dev {
    int vid;
    struct rte_ether_addr mac;
    uint32_t ip;

    // ready if the MAC address has been set
    volatile uint8_t ready;
} __rte_cache_aligned;

#define WINDOW_SIZE 10 // number of intervals (~seconds)
// Per-core runtime state (NO sharing)
struct vdev_rx_stats {
    uint32_t call_count;
    uint32_t pkt_count;
    uint32_t empty_poll_count;
    uint32_t max_poll_count;
    uint32_t ring_enq_fail_count;
    uint32_t byte_wnd[WINDOW_SIZE];
    uint32_t pkt_wnd[WINDOW_SIZE];
    uint32_t empty_wnd[WINDOW_SIZE];
    int wnd_idx; // current window idx
};

struct vdev_tx_stats {
    uint32_t call_count;
    uint32_t pkt_count;
    uint32_t max_send_count;
    uint32_t send_fail_count;
    uint32_t ring_deq_max_count;
};

struct route_table {
    struct rte_hash *mac_2_vid;
    struct rte_hash *ip_2_vid;
};

// struct vhost_dev { // vhost device
//     // Device MAC address (Obtained on first TX packet).
//     struct rte_ether_addr mac_address;
//     uint32_t vm_ip_address;
//     /**< A device is set as ready if the MAC address has been set. */
//     volatile uint8_t ready;
//     /**< Device is marked for removal from the data core. */
//     volatile uint8_t remove;

//     int vid;                      // vhost device ID, assigned by dpdk
//     uint64_t features;            // Virtio feature flags
//     size_t hdr_len;               // Header length
//     struct rte_vhost_memory *mem; // Guest memory mapping

//     // Rate-limited logging for failed enqueue attempts
//     uint64_t last_failed_log_ts; // TSC timestamp of last log
//     uint64_t failed_pkts_count;  // Cumulative failed packets since last log

//     // Track consecutive empty polls before marking device inactive
//     uint8_t empty_poll_count;
//     uint8_t is_active;
// } __rte_cache_aligned;

int check_device_state(struct vhost_dev *vdev, const char *func);
void unregister_vhost_drivers(int socket_num, const char *path);
int register_vhost_drivers();

int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m);
void unlink_vmdq(struct vhost_dev *vdev);

int init_vhost_plans();
int vhost_rx_plan_add(int vid);
int vhost_rx_plan_remove(int vid);
int vhost_tx_plan_add(int vid);
int vhost_tx_plan_remove(int vid);

int init_route_table();
int cleanup_route_table();
int add_route_entry(int vid, struct rte_ether_addr *mac, uint32_t ip);
int remove_route_entry(int vid, struct rte_ether_addr *mac, uint32_t ip);
// use vid to get vdev by indexing the vdev_list global variable
int find_vid_by_mac(struct rte_ether_addr *mac);
int find_vid_by_ip(uint32_t ip);

// static inline unsigned is_route_added()

// static inline unsigned vhost_send(struct dataplane_context *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts)
// {
//     num = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
//     if (num == 0)
//         return 0;

//     STATS_ADD(ctx, pkt_vhost_tx, num);
//     STATS_ADD(ctx, call_vhost_tx, 1);
//     LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->id, vid, num);
//     PRINT_PKTS(pkts, num, LOG_VM_OUT);

//     return num;
// }

#define PERTHREAD_MBUFS 2048
#define BUFFER_SIZE 2048
#define MBUF_SIZE (BUFFER_SIZE + RTE_PKTMBUF_HEADROOM)

static inline struct rte_mempool *vhost_mempool_alloc() {
    static _Atomic unsigned pool_id;
    unsigned n = atomic_fetch_add(&pool_id, 1);

    char name[32];
    snprintf(name, sizeof(name), "mempool_vhost_%u", n);

    struct rte_mempool *mp =
        rte_mempool_create(name, PERTHREAD_MBUFS, MBUF_SIZE, 32, sizeof(struct rte_pktmbuf_pool_private),
                           rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL, rte_socket_id(), 0);

    if (mp == NULL) {
        LOG_ERROR("Failed to create mempool %s: %s\n", name, rte_strerror(rte_errno));
        return NULL;
    }

    return mp;
}
#endif