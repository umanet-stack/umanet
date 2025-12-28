#ifndef STATE_H_
#define STATE_H_

#include <rte_ring.h>

#define ETH_TX_CORES 2
#define ETH_RX_CORES 2
#define VHOST_TX_CORES 2
#define VHOST_RX_CORES 2
#define MAX_VHOSTS 64

#define RING_SIZE 4096

struct dataplane_topology {
    struct rte_ring *eth_tx_rings[ETH_TX_CORES];
    struct rte_ring *vhost_tx_rings[MAX_VHOSTS];
    uint16_t eth_port_id;
};

extern struct dataplane_topology *global;

struct rx_core_ctx {
    uint16_t lcore_id;
    struct route_table *rt; // read-only snapshot
};

struct eth_tx_core_ctx {
    uint16_t lcore_id;
    struct rte_ring *ring;
    uint16_t nr_rings;
};

struct control_ctx {
    struct arp_table *arp;
    struct route_table *rt;
    struct vhost_map *vmap;
};
#endif /* STATE_H_ */