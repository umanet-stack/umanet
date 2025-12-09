/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <rte_vhost.h>
#include <sys/queue.h>

// rte = runtime env (dpdk)
#include <rte_ether.h>

/* Macros for printing using RTE_LOG */
// vhost ops log types: config/data/port
#define RTE_LOGTYPE_VHOST_CONFIG RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_VHOST_DATA RTE_LOGTYPE_USER2
#define RTE_LOGTYPE_VHOST_PORT RTE_LOGTYPE_USER3

// queue type identifiers: receive, transmit, total count
enum { VIRTIO_RXQ, VIRTIO_TXQ, VIRTIO_QNUM };

#define MAX_PKT_BURST 32 /* Max packets processed per burst (RX/TX) */

struct device_statistics {
    uint64_t tx;
    uint64_t tx_total;
    rte_atomic64_t rx_atomic;
    rte_atomic64_t rx_total_atomic;
};

// https://www.redhat.com/en/blog/journey-vhost-users-realm
// https://www.redhat.com/en/blog/virtqueues-and-virtio-ring-how-data-travels
struct vhost_queue {
    struct rte_vhost_vring vr; // DPDK vhost vring
    uint16_t last_avail_idx;   // last processed available descriptor index
    uint16_t last_used_idx;    // last processed used descriptor index
};

struct vhost_dev { // vhost device
    /**< Number of memory regions for gpa to hpa translation. */
    uint32_t nregions_hpa;
    /**< Device MAC address (Obtained on first TX packet). */
    struct rte_ether_addr mac_address;
    /**< RX VMDQ (VM device queue) queue number. */
    uint16_t vmdq_rx_q; // stores the RX queue number assigned to each vhost device
    /**< Vlan tag assigned to the pool */
    uint32_t vlan_tag;
    /**< Data core that the device is added to. */
    uint16_t coreid;
    /**< A device is set as ready if the MAC address has been set. */
    volatile uint8_t ready;
    /**< Device is marked for removal from the data core. */
    volatile uint8_t remove;

    int vid;                      // vhost device ID
    uint64_t features;            // Virtio feature flags
    size_t hdr_len;               // Header length
    uint16_t nr_vrings;           // Number of virtio rings
    struct rte_vhost_memory *mem; // Guest memory mapping
    struct device_statistics stats;
    TAILQ_ENTRY(vhost_dev) global_vdev_entry; // Global list entry
    TAILQ_ENTRY(vhost_dev) lcore_vdev_entry;  // Per-core list entry

#define MAX_QUEUE_PAIRS 4
    struct vhost_queue queues[MAX_QUEUE_PAIRS * 2]; // 4 pairs of RX/TX queues

    // NEW: TCP offload support
    int tcp_offload_enabled;
    struct tcp_flow_state *flows; // Flows associated with this VM
} __rte_cache_aligned;

TAILQ_HEAD(vhost_dev_tailq_list, vhost_dev);

#define REQUEST_DEV_REMOVAL 1
#define ACK_DEV_REMOVAL 0

/*
 * Structure containing data core specific information.
 */
struct lcore_info {
    uint32_t device_num;

    /* Flag to synchronize device removal. */
    volatile uint8_t dev_removal_flag;

    // list of devices on this core
    struct vhost_dev_tailq_list vdev_list;
};

/* we implement non-extra virtio net features (0 = no extra features) */
#define VIRTIO_NET_FEATURES 0

// vs = vhost-server
void vs_vhost_net_setup(struct vhost_dev *dev);
void vs_vhost_net_remove(struct vhost_dev *dev);
uint16_t vs_enqueue_pkts(struct vhost_dev *dev, uint16_t queue_id, struct rte_mbuf **pkts, uint32_t count);

uint16_t vs_dequeue_pkts(struct vhost_dev *dev, uint16_t queue_id, struct rte_mempool *mbuf_pool,
                         struct rte_mbuf **pkts, uint16_t count);
#endif /* _MAIN_H_ */
