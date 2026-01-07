#include "log.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/slow/slowpath.h"
#include "src/vhost/vhost.h"
#include <rte_lcore.h>
#include <rte_malloc.h>

int init_dataplane_topology() {
    if ((global = rte_calloc("global", 1, sizeof(*global), 0)) == NULL) {
        LOG_ERROR("dataplane_init: failed to allocate global\n");
        return -1;
    }
    if (config.eth_tx_queues > MAX_ETH_TX_QUEUES) {
        LOG_ERROR("init_dataplane_topology: eth_tx_queues (%d) > MAX_ETH_TX_QUEUES (%d)\n", config.eth_tx_queues,
                  MAX_ETH_TX_QUEUES);
        return -1;
    }
    global->fp_cores = config.eth_rx_cores + config.eth_tx_cores + config.vhost_rx_cores + config.vhost_tx_cores;

    return 0;
}

int init_dataplane_ctxs() {
    if ((eth_rx_ctxs = rte_calloc("eth_rx_ctxs", config.eth_rx_cores, sizeof(*eth_rx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_rx_ctxs\n");
        return -1;
    }
    if ((eth_tx_ctxs = rte_calloc("eth_tx_ctxs", config.eth_tx_cores, sizeof(*eth_tx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate eth_tx_ctxs\n");
        return -1;
    }
    if ((vhost_rx_ctxs = rte_calloc("vhost_rx_ctxs", config.vhost_rx_cores, sizeof(*vhost_rx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_rx_ctxs\n");
        return -1;
    }
    if ((vhost_tx_ctxs = rte_calloc("vhost_tx_ctxs", config.vhost_tx_cores, sizeof(*vhost_tx_ctxs), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate vhost_tx_ctxs\n");
        return -1;
    }

    for (int i = 0; i < config.eth_rx_cores; i++) {
        if ((eth_rx_ctxs[i] = rte_calloc("eth_rx_ctxs[%d]", 1, sizeof(*eth_rx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]\n", i);
            return -1;
        }
        eth_rx_ctxs[i]->eth_rx_queue_r = i;

        if ((eth_rx_ctxs[i]->mempool = network_mempool_alloc()) == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]->mempool\n", i);
            rte_free(eth_rx_ctxs[i]);
            return -1;
        }

        if ((eth_rx_ctxs[i]->stats = rte_calloc("eth_rx_ctxs[%d]->stats", 1, sizeof(*eth_rx_ctxs[i]->stats), 0)) ==
            NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to allocate eth_rx_ctxs[%d]->stats\n", i);
            return -1;
        }
    }

    for (int i = 0; i < config.eth_tx_cores; i++) {
        if ((eth_tx_ctxs[i] = rte_calloc("eth_tx_ctxs[%d]", 1, sizeof(*eth_tx_ctxs[i]), 0)) == NULL) {
            LOG_ERROR("init_eth_tx_ctxs: failed to allocate eth_tx_ctxs[%d]\n", i);
            return -1;
        }
        eth_tx_ctxs[i]->eth_tx_queue_r = i;

        eth_tx_ctxs[i]->gro_param = (struct rte_gro_param){.gro_types = RTE_GRO_TCP_IPV4,
                                                           .max_flow_num = GRO_MAX_FLOWS,
                                                           .max_item_per_flow = GRO_MAX_ITEMS_PER_FLOW,
                                                           .socket_id = rte_socket_id()};
        eth_tx_ctxs[i]->gro_ctx = rte_gro_ctx_create(&eth_tx_ctxs[i]->gro_param);
        if (eth_tx_ctxs[i]->gro_ctx == NULL) {
            LOG_ERROR("init_eth_rx_ctxs: failed to create GRO context\n");
            return -1;
        }

        if ((eth_tx_ctxs[i]->stats = rte_calloc("eth_tx_ctxs[%d]->stats", 1, sizeof(*eth_tx_ctxs[i]->stats), 0)) ==
            NULL) {
            LOG_ERROR("init_eth_tx_ctxs: failed to allocate eth_tx_ctxs[%d]->stats\n", i);
            return -1;
        }
    }

    for (int i = 0; i < config.vhost_rx_cores; i++) {
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
            vhost_rx_ctxs[i]->vhost_ap[j].state = RX_HOT;
            vhost_rx_ctxs[i]->vhost_ap[j].idle_score = 0;
            vhost_rx_ctxs[i]->vhost_ap[j].blocked_until_tsc = 0;
        }
        memset(vhost_rx_ctxs[i]->poll_states, 0, sizeof(vhost_rx_ctxs[i]->poll_states));
        memset(vhost_rx_ctxs[i]->ecn_rr_vhost, 0, sizeof(vhost_rx_ctxs[i]->ecn_rr_vhost));
        memset(vhost_rx_ctxs[i]->ecn_rr_eth, 0, sizeof(vhost_rx_ctxs[i]->ecn_rr_eth));
    }

    for (int i = 0; i < config.vhost_tx_cores; i++) {
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
            // retry_pkts is now a static array, no need to initialize
            vhost_tx_ctxs[i]->retry_cnts[j] = 0;
            vhost_tx_ctxs[i]->vm_bp[j].state = VM_ACTIVE;
            vhost_tx_ctxs[i]->vm_bp[j].blocked_until_tsc = 0;
        }
    }
    // vhost module takes care of vdev_ids

    if ((control_ctx = rte_calloc("control_ctx", 1, sizeof(struct control_ctx), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to allocate control_ctxs\n");
        return -1;
    }
    control_ctx->core_id = 0;
    if ((control_ctx->msg_pool =
             rte_mempool_create("control_ctx->msg_pool", PERTHREAD_MBUFS, MBUF_SIZE, 32, sizeof(struct slow_msg),
                                rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL, rte_socket_id(), 0)) == NULL) {
        LOG_ERROR("init_dataplane_ctxs: failed to create control_ctx->msg_pool\n");
        return -1;
    }

    return 0;
}