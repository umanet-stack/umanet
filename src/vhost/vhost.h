/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef _VHOST_H_
#define _VHOST_H_

#include "../include/tas.h"
#include "src/include/fastpath.h"
#include <rte_ether.h>
#include <rte_vhost.h>
#include <sys/queue.h>

#include "src/utils/utils.h"

// rte = runtime env (dpdk)
// queue type identifiers: receive, transmit, total count
enum { VIRTIO_RXQ, VIRTIO_TXQ, VIRTIO_QNUM };

// https://www.redhat.com/en/blog/journey-vhost-users-realm
// https://www.redhat.com/en/blog/virtqueues-and-virtio-ring-how-data-travels
// struct vhost_queue {
//     struct rte_vhost_vring vr; // DPDK vhost vring
//     uint16_t last_avail_idx;   // last processed available descriptor index
//     uint16_t last_used_idx;    // last processed used descriptor index
// };

#define REQUEST_DEV_REMOVAL 1
#define ACK_DEV_REMOVAL 0

#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MBUF_TABLE_DRAIN_TSC ((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US)

// typedef struct {
//     unsigned dev_to_core_id[64];
// } vhost_state_t;

extern const struct vhost_device_ops virtio_net_device_ops;
extern const uint16_t vlan_tags[64];

// Forward declarations
void free_pkts(struct rte_mbuf **pkts, uint16_t n);
void log_pkt_in(const char *fmt, ...);
void log_pkt_out(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_warn(const char *fmt, ...);

void poll_virtio_tx(struct vhost_dev *vdev, struct dataplane_context *ctx);

void flush_eth_tx(struct mbuf_table *tx_q);
void poll_eth_rx(struct vhost_dev *vdev);

struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac);
void unregister_vhost_drivers(int socket_num, const char *path);
int register_vhost_drivers();

int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m);
void unlink_vmdq(struct vhost_dev *vdev);

static inline unsigned vhost_poll(struct network_thread *t, unsigned num, unsigned vid, struct rte_mbuf **mbs) {
    num = rte_vhost_dequeue_burst(vid, VIRTIO_TXQ, t->pool, mbs, num);

    return num;
}

#endif