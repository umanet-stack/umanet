/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef VHOST_H_
#define VHOST_H_

#include <rte_ether.h>
#include <rte_vhost.h>
#include <sys/queue.h>

#include "log.h"
#include "src/include/fastpath.h"

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

extern const struct rte_vhost_device_ops virtio_net_device_ops;

int check_device_state(struct vhost_dev *vdev, const char *func);
struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac);
struct vhost_dev *find_vhost_dev_core(struct dataplane_context *ctx, struct rte_ether_addr *mac);
void unregister_vhost_drivers(int socket_num, const char *path);
int register_vhost_drivers();

int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m);
void unlink_vmdq(struct dataplane_context *ctx, struct vhost_dev *vdev);

// copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
// This can fail if the vhost connection is broken
static inline unsigned vhost_poll(struct dataplane_context *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    num = rte_vhost_dequeue_burst(vid, VIRTIO_TXQ, ctx->net.pool, pkts, num);
    if (num == 0)
        return 0;

    STATS_ADD(ctx, pkt_vhost_rx, num);
    STATS_ADD(ctx, call_vhost_rx, 1);
    LOG_VM_IN("[%d](%d) Received %d packets from VM\n", ctx->id, vid, num);
    PRINT_PKTS(pkts, num, LOG_VM_IN);

    return num;
}

static inline unsigned vhost_send(struct dataplane_context *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    num = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    if (num == 0)
        return 0;

    STATS_ADD(ctx, pkt_vhost_tx, num);
    STATS_ADD(ctx, call_vhost_tx, 1);
    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->id, vid, num);
    PRINT_PKTS(pkts, num, LOG_VM_OUT);

    return num;
}

#endif