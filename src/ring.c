#include "src/include/main.h"
#include "src/include/state.h"
#include <rte_ring.h>

int init_rings() {
    for (int i = 0; i < config.eth_tx_cores; i++) {
        char *name = malloc(sizeof(char) * 100);
        sprintf(name, "eth_tx_ring_%d", i);
        global->eth_tx_rings[i] = rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_MP_RTS_ENQ | RING_F_SC_DEQ);
        free(name);
    }
    for (int i = 0; i < config.nb_sockets; i++) {
        char *name = malloc(sizeof(char) * 100);
        sprintf(name, "vhost_tx_ring_%d", i);
        global->vhost_tx_rings[i] =
            rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_MP_RTS_ENQ | RING_F_SC_DEQ);
        free(name);
    }

    char *name = malloc(sizeof(char) * 100);
    sprintf(name, "slowpath_ring");
    global->slowpath_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_MP_RTS_ENQ | RING_F_SC_DEQ);
    free(name);

    return 0;
}

void destroy_rings() {
    for (int i = 0; i < config.eth_tx_cores; i++) {
        rte_ring_free(global->eth_tx_rings[i]);
        free(global->eth_tx_rings[i]);
    }
    for (int i = 0; i < config.nb_sockets; i++) {
        rte_ring_free(global->vhost_tx_rings[i]);
        free(global->vhost_tx_rings[i]);
    }
    rte_ring_free(global->slowpath_ring);
    free(global->slowpath_ring);
}