#ifndef STATE_H_
#define STATE_H_

#include "src/fast/poll.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"
#include <rte_ether.h>
#include <rte_gro.h>
#include <rte_hash.h>
#include <rte_ring.h>
#include <stdatomic.h>

#define MAX_VHOSTS 64

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

#define MAX_ETH_TX_QUEUES 20

struct dataplane_topology {
    uint16_t eth_port_id;
    struct rte_ether_addr eth_addr;
    uint16_t fp_cores;

    // vhost_rx_core i => eth_tx_queue_rings[j] => eth_tx_core k (i:j:k mapping)
    struct rte_ring **eth_tx_queue_rings;

    // indexed by vid
    // vhost/eth_rx_core i => vhost_tx_rings[j] => vhost_tx_core k (i:j:k mapping)
    struct rte_ring *vhost_tx_rings[MAX_VHOSTS];
    struct rte_ring *slowpath_ring;
};

struct eth_rx_ctx {
    uint16_t core_id;
    // e.g. 0 => get pkts from eth_rx_queue_rings[0, n, 2n, ...]
    // send to NIC tx queue 0, n, 2n, ...
    uint16_t eth_rx_queue_r;
    struct rte_mempool *mempool;
    struct eth_rx_stats *stats;
    struct rte_gro_param gro_param;
    void *gro_ctx;
    uint64_t iteration_counter;
};

struct eth_tx_ctx {
    uint16_t core_id;
    // e.g. 0 => get pkts from eth_tx_queue_rings[0, n, 2n, ...]
    // send to NIC rx queue 0, n, 2n, ...
    uint16_t eth_tx_queue_r;
    struct eth_tx_stats *stats;
    struct rte_gro_param gro_param;
    void *gro_ctx;
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
    uint32_t requeue_pkt_count;
    uint32_t max_send_count;
    uint32_t ring_deq_max_count;
};

enum vm_state {
    VM_ACTIVE,
    VM_BLOCKED_TX,
    VM_IDLE_RX,
};

#define BACKOFF_TSC 100000 // 100 us
// backpressure for vms
struct vm_bp {
    enum vm_state state;
    uint64_t blocked_until_tsc;
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
    // Global iteration counter for adaptive polling
    uint64_t iteration_counter;

    struct vdev_rx_stats *vdev_stats[MAX_VHOSTS];
    struct vhost_ap vhost_ap[MAX_VHOSTS];
    uint32_t poll_states[5];
    uint32_t ecn_rr_vhost[MAX_VHOSTS];
    uint32_t ecn_rr_eth[MAX_ETH_TX_QUEUES];
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
    // Persistent storage for packets waiting to be retried
    struct rte_mbuf *retry_pkts[MAX_VHOSTS][MAX_PKT_BURST];
    uint32_t retry_cnts[MAX_VHOSTS];
    struct vm_bp vm_bp[MAX_VHOSTS];
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
    struct rte_mempool *msg_pool;
    // struct arp_table *arp;
    // struct route_table *route_table;
    // struct vhost_map *vmap;
};

#endif /* STATE_H_ */