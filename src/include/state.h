#ifndef STATE_H_
#define STATE_H_

#include <rte_ether.h>
#include <rte_ring.h>

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

struct dataplane_topology {
    uint16_t eth_port_id;
    struct rte_ether_addr eth_addr;

    struct rte_ring *eth_tx_rings[ETH_TX_CORES];
    struct rte_ring *vhost_tx_rings[MAX_VHOSTS];

    uint16_t eth_tx_cores;
    uint16_t eth_rx_cores;
    uint16_t vhost_tx_cores;
    uint16_t vhost_rx_cores;
    uint16_t fp_cores;
};

struct eth_rx_ctx {
    uint16_t id;
    uint16_t eth_queue_id; // same as id
    // struct route_table *rt; // read-only snapshot
};

struct eth_tx_ctx {
    uint16_t id;
    uint16_t eth_queue_id; // same as id
};

struct vhost_rx_ctx {
    uint16_t id;
    uint16_t vdev_ids[MAX_VHOSTS];
    uint16_t num_vdevs;
    struct rte_mempool *mempool;
};

struct vhost_tx_ctx {
    uint16_t id;
    uint16_t vdev_ids[MAX_VHOSTS];
    uint16_t num_vdevs;
};

struct control_ctx {
    struct arp_table *arp;
    struct route_table *rt;
    struct vhost_map *vmap;
};
#endif /* STATE_H_ */