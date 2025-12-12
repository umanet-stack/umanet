/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef _VHOST_H_
#define _VHOST_H_

#include "../include/tas.h"

/* State of virtio device. */
#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MBUF_TABLE_DRAIN_TSC ((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US)
#define VLAN_HLEN 4

/* Used for queueing bursts of TX packets. */
struct mbuf_table {
    unsigned len;
    unsigned txq_id;
    struct rte_mbuf *m_table[MAX_PKT_BURST];
};

typedef struct {
    struct lcore_info lcore_info[RTE_MAX_LCORE];
    struct vhost_dev_tailq_list vhost_dev_list;

    /* TX queue for each data core. */
    struct mbuf_table lcore_tx_queue[RTE_MAX_LCORE];

    unsigned lcore_ids[RTE_MAX_LCORE];
} vhost_state_t;

extern vhost_state_t vhost;
extern const struct vhost_device_ops virtio_net_device_ops;

struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac);
int switch_worker(void *arg __rte_unused);
void create_mbuf_pool(uint16_t nr_port, uint32_t nr_switch_core, uint32_t mbuf_size, uint32_t nr_queues,
                      uint32_t nr_rx_desc, uint32_t nr_mbuf_cache);
void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, uint16_t vlan_tag);
int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m);
void unlink_vmdq(struct vhost_dev *vdev);
void free_pkts(struct rte_mbuf **pkts, uint16_t n);
void do_drain_mbuf_table(struct mbuf_table *tx_q);

void drain_virtio_tx(struct vhost_dev *vdev);
void drain_eth_rx(struct vhost_dev *vdev);
void drain_mbuf_table(struct mbuf_table *tx_q);
#endif