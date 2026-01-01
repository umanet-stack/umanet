#ifndef STATE_H_
#define STATE_H_

#include "src/vhost/vhost.h"
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_ring.h>
#include <stdatomic.h>

#define ETH_TX_CORES 2
#define ETH_RX_CORES 2
#define VHOST_TX_CORES 2
#define VHOST_RX_CORES 2
#define FP_CORES ETH_TX_CORES + ETH_RX_CORES + VHOST_TX_CORES + VHOST_RX_CORES
#define MAX_VHOSTS 64

#define RING_SIZE 4096

extern struct dataplane_topology *global;
extern struct eth_rx_ctx **eth_rx_ctxs;
extern struct eth_tx_ctx **eth_tx_ctxs;
extern struct vhost_rx_ctx **vhost_rx_ctxs;
extern struct vhost_tx_ctx **vhost_tx_ctxs;
extern struct control_ctx *control_ctx;
extern _Atomic(struct vdev_list *) vdev_list;
extern _Atomic(struct vhost_plan *) *vhost_rx_plans;
extern _Atomic(struct vhost_plan *) *vhost_tx_plans;
extern uint16_t vhost_rx_core[MAX_VHOSTS];
extern uint16_t vhost_tx_core[MAX_VHOSTS];

struct dataplane_topology {
    uint16_t eth_port_id;
    struct rte_ether_addr eth_addr;

    // indexed by eth_queue_id
    struct rte_ring *eth_tx_rings[ETH_TX_CORES];
    // indexed by vid
    struct rte_ring *vhost_tx_rings[MAX_VHOSTS];
    struct rte_ring *slowpath_ring;

    uint16_t eth_tx_cores;
    uint16_t eth_rx_cores;
    uint16_t vhost_tx_cores;
    uint16_t vhost_rx_cores;
    uint16_t fp_cores;
};

struct eth_rx_ctx {
    uint16_t core_id;
    uint16_t eth_queue_id; // same as core_id
    struct rte_mempool *mempool;
    struct eth_rx_stats *stats;
};

struct eth_tx_ctx {
    uint16_t core_id;
    uint16_t eth_queue_id; // same as core_id
    struct eth_tx_stats *stats;
};

// Per-core runtime state (NO sharing)
struct eth_rx_stats {
    uint32_t call_count;
    uint32_t pkt_count;
    uint32_t empty_poll_count;
    uint32_t max_poll_count;
    uint32_t ring_enq_fail_count;
};

struct eth_tx_stats {
    uint32_t call_count;
    uint32_t pkt_count;
    uint32_t max_send_count;
    uint32_t send_fail_count;
    uint32_t ring_deq_max_count;
};

struct vhost_rx_ctx {
    uint16_t core_id;
    uint16_t vhost_rx_core_id;
    struct rte_mempool *mempool;
    /* Flag to synchronize device removal. */
    volatile uint8_t dev_removal_flag;
    // Round-robin index for polling devices
    uint16_t next_device;
    // Counter for checking inactive devices
    uint16_t inactive_check_counter;

    struct vdev_rx_stats *vdev_stats[MAX_VHOSTS];
};

struct vhost_tx_ctx {
    uint16_t core_id;
    uint16_t vhost_tx_core_id;
    /* Flag to synchronize device removal. */
    volatile uint8_t dev_removal_flag;
    // Round-robin index for polling devices
    uint16_t next_device;
    // Counter for checking inactive devices
    uint16_t inactive_check_counter;

    struct vdev_tx_stats *vdev_stats[MAX_VHOSTS];
};

// Published via atomic pointer swap, Never mutated, RX/TX cores only read
struct vdev_list {
    uint16_t num;
    // indexed by vid
    struct vhost_dev *vdevs[MAX_VHOSTS];
} __rte_cache_aligned;

// poll/send plan generated from vdev_list, read by vhost RX/TX cores
struct vhost_plan {
    uint16_t num;
    uint16_t vids[MAX_VHOSTS];
} __rte_cache_aligned;

struct control_ctx {
    uint16_t core_id;
    // struct arp_table *arp;
    // struct route_table *route_table;
    // struct vhost_map *vmap;
};

#endif /* STATE_H_ */