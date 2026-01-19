/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef VHOST_H_
#define VHOST_H_

#include "log.h"
#include "src/include/fastpath.h"
#include <rte_errno.h>
#include <rte_ether.h>
#include <rte_vhost.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/queue.h>

enum { VIRTIO_RXQ, VIRTIO_TXQ };

#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

extern const struct rte_vhost_device_ops virtio_net_device_ops;
extern struct route_table route_table;

struct vhost_feature {
    uint64_t bit;
    const char *name;
};

extern struct vhost_feature features[];

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
    // vid != vm_id as vid is FIFO order and server vms are spawned first
    int vm_id;
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
    uint32_t vhost_tx_ring_enq_fail_count;
    uint32_t eth_tx_ring_enq_fail_count;
    uint32_t byte_wnd[WINDOW_SIZE];
    uint32_t pkt_wnd[WINDOW_SIZE];
    uint32_t empty_wnd[WINDOW_SIZE];
    int wnd_idx; // current window idx
};

struct vdev_tx_stats {
    uint32_t call_count;
    uint32_t pkt_count;
    uint32_t max_send_count;
    uint32_t requeue_pkt_count;
    uint32_t ring_deq_max_count;
};

struct route_table {
    struct rte_hash *mac_2_vid;
    struct rte_hash *ip_2_vid;
};

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

extern int ip_last_octet_to_vid[256];
void init_route_table();
int add_route_entry(int vid, uint32_t ip);
int remove_route_entry(uint32_t ip);

static inline int find_vid_by_ip(uint32_t ip) {
    // last octet (192.168.100.X -> X)
    uint8_t last_octet = ip & 0xFF;
    // -1 = not found
    return ip_last_octet_to_vid[last_octet];
}

// int cleanup_route_table();
// int remove_route_entry(int vid, struct rte_ether_addr *mac, uint32_t ip);
// use vid to get vdev by indexing the vdev_list global variable
// int find_vid_by_mac(struct rte_ether_addr *mac);
#endif