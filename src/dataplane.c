#include "log.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"
#include <rte_malloc.h>

int init_dataplane_topology() {
    if ((global = rte_calloc("global", 1, sizeof(*global), 0)) == NULL) {
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

int init_dataplane_ctxs() {
    if ((eth_rx_ctxs = rte_calloc("eth_rx_ctxs", global->eth_rx_cores, sizeof(*eth_rx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_rx_ctxs\n");
        return -1;
    }
    if ((eth_tx_ctxs = rte_calloc("eth_tx_ctxs", global->eth_tx_cores, sizeof(*eth_tx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_tx_ctxs\n");
        return -1;
    }
    if ((vhost_rx_ctxs = rte_calloc("vhost_rx_ctxs", global->vhost_rx_cores, sizeof(*vhost_rx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_rx_ctxs\n");
        return -1;
    }
    if ((vhost_tx_ctxs = rte_calloc("vhost_tx_ctxs", global->vhost_tx_cores, sizeof(*vhost_tx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_tx_ctxs\n");
        return -1;
    }

    for (int i = 0; i < global->eth_rx_cores; i++) {
        if ((eth_rx_ctxs[i] = rte_calloc("eth_rx_ctxs[%d]", 1, sizeof(*eth_rx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]\n", i);
            return -1;
        }
        eth_rx_ctxs[i]->eth_queue_id = i;

        if ((eth_rx_ctxs[i]->mempool = network_mempool_alloc()) == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]->mempool\n", i);
            rte_free(eth_rx_ctxs[i]);
            return -1;
        }
    }

    for (int i = 0; i < global->eth_tx_cores; i++) {
        if ((eth_tx_ctxs[i] = rte_calloc("eth_tx_ctxs[%d]", 1, sizeof(*eth_tx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_eth_tx_ctxs: failed to allocate eth_tx_ctxs[%d]\n", i);
            return -1;
        }
        eth_tx_ctxs[i]->eth_queue_id = i;
    }

    for (int i = 0; i < global->vhost_rx_cores; i++) {
        if ((vhost_rx_ctxs[i] = rte_calloc("vhost_rx_ctxs[%d]", 1, sizeof(*vhost_rx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_vhost_rx_ctxs: failed to allocate vhost_rx_ctxs[%d]\n", i);
            return -1;
        }
        vhost_rx_ctxs[i]->vhost_rx_core_id = i;

        if ((vhost_rx_ctxs[i]->mempool = vhost_mempool_alloc()) == NULL) {
            LOG_ERROR("init_vhost_rx_ctxs: failed to allocate vhost_rx_ctxs[%d]->mempool\n", i);
            rte_free(vhost_rx_ctxs[i]);
            return -1;
        }

        // Allocate individual vdev_stats structs (vdev_stats is already an array of pointers)
        for (int j = 0; j < MAX_VHOSTS; j++) {
            vhost_rx_ctxs[i]->vdev_stats[j] =
                rte_zmalloc("vhost_rx_ctxs[%d]->vdev_stats[%d]", sizeof(struct vdev_rx_stats), RTE_CACHE_LINE_SIZE);
            if (vhost_rx_ctxs[i]->vdev_stats[j] == NULL) {
                LOG_ERROR("init_vhost_rx_ctxs: failed to allocate vhost_rx_ctxs[%d]->vdev_stats[%d]\n", i, j);
                return -1;
            }
        }
    }

    for (int i = 0; i < global->vhost_tx_cores; i++) {
        if ((vhost_tx_ctxs[i] = rte_calloc("vhost_tx_ctxs[%d]", 1, sizeof(*vhost_tx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_vhost_tx_ctxs: failed to allocate vhost_tx_ctxs[%d]\n", i);
            return -1;
        }
        vhost_tx_ctxs[i]->vhost_tx_core_id = i;

        for (int j = 0; j < MAX_VHOSTS; j++) {
            vhost_tx_ctxs[i]->vdev_stats[j] =
                rte_zmalloc("vhost_tx_ctxs[%d]->vdev_stats[%d]", sizeof(struct vdev_tx_stats), RTE_CACHE_LINE_SIZE);
            if (vhost_tx_ctxs[i]->vdev_stats[j] == NULL) {
                LOG_ERROR("init_vhost_tx_ctxs: failed to allocate vhost_tx_ctxs[%d]->vdev_stats[%d]\n", i, j);
                return -1;
            }
        }
    }
    // vhost module takes care of vdev_ids

    if ((control_ctx = rte_calloc("control_ctx", 1, sizeof(struct control_ctx), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate control_ctxs\n");
        return -1;
    }
    control_ctx->core_id = 0;

    return 0;
}