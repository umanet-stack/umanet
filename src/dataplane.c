#include "log.h"
#include "src/include/state.h"

int init_dataplane_topology(void) {
    if ((global = calloc(1, sizeof(*global))) == NULL) {
        LOG_ERROR("dataplane_init: failed to allocate global\n");
        return -1;
    }

    global->eth_tx_cores = ETH_TX_CORES;
    global->eth_rx_cores = ETH_RX_CORES;
    global->vhost_tx_cores = VHOST_TX_CORES;
    global->vhost_rx_cores = VHOST_RX_CORES;
    global->fp_cores = FP_CORES;

    return 0;
}

int init_dataplane_ctxs(void) {
    if ((eth_rx_ctxs = calloc(global->eth_rx_cores, sizeof(*eth_rx_ctxs))) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_rx_ctxs\n");
        return -1;
    }
    if ((eth_tx_ctxs = calloc(global->eth_tx_cores, sizeof(*eth_tx_ctxs))) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_tx_ctxs\n");
        return -1;
    }
    if ((vhost_rx_ctxs = calloc(global->vhost_rx_cores, sizeof(*vhost_rx_ctxs))) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_rx_ctxs\n");
        return -1;
    }
    if ((vhost_tx_ctxs = calloc(global->vhost_tx_cores, sizeof(*vhost_tx_ctxs))) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_tx_ctxs\n");
        return -1;
    }

    for (int i = 0; i < global->eth_rx_cores; i++) {
        if ((eth_rx_ctxs[i] = calloc(1, sizeof(*eth_rx_ctxs[i]))) == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]\n", i);
            return -1;
        }
        eth_rx_ctxs[i]->id = i;
        eth_rx_ctxs[i]->eth_queue_id = i;
    }

    for (int i = 0; i < global->eth_tx_cores; i++) {
        if ((eth_tx_ctxs[i] = calloc(1, sizeof(*eth_tx_ctxs[i]))) == NULL) {
            LOG_ERROR("init_eth_tx_ctxs: failed to allocate eth_tx_ctxs[%d]\n", i);
            return -1;
        }
        eth_tx_ctxs[i]->id = i;
        eth_tx_ctxs[i]->eth_queue_id = i;
    }

    for (int i = 0; i < global->vhost_rx_cores; i++) {
        if ((vhost_rx_ctxs[i] = calloc(1, sizeof(*vhost_rx_ctxs[i]))) == NULL) {
            LOG_ERROR("init_vhost_rx_ctxs: failed to allocate vhost_rx_ctxs[%d]\n", i);
            return -1;
        }
        vhost_rx_ctxs[i]->id = i;
        vhost_rx_ctxs[i]->mempool = NULL;
        vhost_rx_ctxs[i]->num_vdevs = 0;
    }

    for (int i = 0; i < global->vhost_tx_cores; i++) {
        if ((vhost_tx_ctxs[i] = calloc(1, sizeof(*vhost_tx_ctxs[i]))) == NULL) {
            LOG_ERROR("init_vhost_tx_ctxs: failed to allocate vhost_tx_ctxs[%d]\n", i);
            return -1;
        }
        vhost_tx_ctxs[i]->id = i;
        vhost_tx_ctxs[i]->num_vdevs = 0;
    }

    return 0;
}