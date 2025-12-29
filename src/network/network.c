/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "log.h"
#include "src/config/config.h"
#include <assert.h>
#include <stdio.h>

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>
#include <rte_version.h>

#include "../fast/internal.h"
#include "../include/main.h"
#include "network.h"
#include "src/include/state.h"
#include <tas_memif.h>
#include <utils.h>
#include <utils_rng.h>

#define RX_DESCRIPTORS 256
#define TX_DESCRIPTORS 128

static struct rte_eth_conf port_conf = {
    .rxmode =
        {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
            .offloads = 0,
        },
    .txmode =
        {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = 0,
        },
    .rx_adv_conf =
        {
            .rss_conf =
                {
                    // Include both TCP and UDP for proper RSS distribution
                    // This ensures both TCP and UDP packets are distributed across queues
                    .rss_hf = RTE_ETH_FLOW_NONFRAG_IPV4_TCP | RTE_ETH_FLOW_NONFRAG_IPV4_UDP,
                },
        },
    .intr_conf =
        {
            .rxq = 1,
        },
};

static struct network_rx_thread **net_threads;

static struct rte_eth_dev_info eth_devinfo;

uint16_t rss_reta_size;
static struct rte_eth_rss_reta_entry64 *rss_reta = NULL;
static uint16_t *rss_core_buckets = NULL;

static struct rte_mempool *mempool_alloc(void);
static int reta_setup(void);
static rte_spinlock_t initlock = RTE_SPINLOCK_INITIALIZER;

int network_init() {
    uint8_t count;
    int ret;
    uint16_t p;

    /* make sure there is only one port */
    count = rte_eth_dev_count_avail();
    if (count == 0) {
        LOG_ERROR("No ethernet devices\n");
        goto error_exit;
    } else if (count > 1) {
        LOG_ERROR("Multiple ethernet devices\n");
        goto error_exit;
    }

    // used -w (whitelist) for NIC PCI addr in dpdk args, this should have only one port with id 0
    RTE_ETH_FOREACH_DEV(p) { global->eth_port_id = p; }
    if (!rte_eth_dev_is_valid_port(global->eth_port_id)) {
        LOG_ERROR("Specified port ID(%u) is not valid\n", global->eth_port_id);
        goto error_exit;
    }

    // get mac address and device info
    rte_eth_macaddr_get(global->eth_port_id, &global->eth_addr);
    rte_eth_dev_info_get(global->eth_port_id, &eth_devinfo);

    if (eth_devinfo.max_rx_queues < global->eth_rx_cores || eth_devinfo.max_tx_queues < global->eth_rx_cores) {
        LOG_ERROR("Error: NIC does not support enough hw queues (rx=%u tx=%u)"
                  " for the requested number of cores (%u)\n",
                  eth_devinfo.max_rx_queues, eth_devinfo.max_tx_queues, global->eth_rx_cores);
        goto error_exit;
    }

    /* mask unsupported RSS hash functions */
    if ((port_conf.rx_adv_conf.rss_conf.rss_hf & eth_devinfo.flow_type_rss_offloads) !=
        port_conf.rx_adv_conf.rss_conf.rss_hf) {
        LOG_WARN("NIC does not support all requested RSS hash functions.\n");
        port_conf.rx_adv_conf.rss_conf.rss_hf &= eth_devinfo.flow_type_rss_offloads;
    }

    /* enable per port checksum offload if requested */
    if (config.fp_xsumoffload) {
        uint64_t requested_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
        /* mask unsupported TX offloads */
        port_conf.txmode.offloads = requested_offloads & eth_devinfo.tx_offload_capa;
        if (port_conf.txmode.offloads != requested_offloads) {
            LOG_WARN("NIC does not support all requested TX offloads (requested: 0x%lx, supported: 0x%lx, "
                     "using: 0x%lx).\n",
                     requested_offloads, eth_devinfo.tx_offload_capa, port_conf.txmode.offloads);
        }
    }

    /* disable rx interrupts if requested */
    if (!config.fp_interrupts)
        port_conf.intr_conf.rxq = 0;

    /* initialize port */
    ret = rte_eth_dev_configure(global->eth_port_id, global->eth_rx_cores, global->eth_tx_cores, &port_conf);
    if (ret < 0) {
        LOG_ERROR("rte_eth_dev_configure failed\n");
        goto error_exit;
    }

    eth_devinfo.default_rxconf.offloads = 0;

    /* enable per-queue checksum offload if requested */
    eth_devinfo.default_txconf.offloads = 0;
    if (config.fp_xsumoffload) {
        uint64_t requested_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
        /* mask unsupported TX offloads (use same mask as port-level) */
        eth_devinfo.default_txconf.offloads = requested_offloads & eth_devinfo.tx_offload_capa;
    }

    return 0;

error_exit:
    rte_free(net_threads);
    return -1;
}

void network_cleanup(void) {
    rte_eth_dev_stop(global->eth_port_id);
    rte_free(net_threads);
}

void network_dump_stats(void) {
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(0, &stats) == 0) {
        fprintf(stderr,
                "network stats: ipackets=%" PRIu64 " opackets=%" PRIu64 " ibytes=%" PRIu64 " obytes=%" PRIu64
                " imissed=%" PRIu64 " ierrors=%" PRIu64 " oerrors=%" PRIu64 " rx_nombuf=%" PRIu64 "\n",
                stats.ipackets, stats.opackets, stats.ibytes, stats.obytes, stats.imissed, stats.ierrors, stats.oerrors,
                stats.rx_nombuf);
    } else {
        fprintf(stderr, "failed to get stats\n");
    }
}

// NIC TX/RX queues id = ctx id, + start eth if core 0
int network_thread_init(struct dataplane_context *ctx) {
    static volatile uint32_t tx_init_done = 0;
    static volatile uint32_t rx_init_done = 0;
    static volatile uint32_t start_done = 0;

    struct network_thread *t = &ctx->net;
    int ret;

    /* allocate mempool */
    if ((t->pool = mempool_alloc()) == NULL) {
        goto error_mpool;
    }
    vhost_rx_ctxs[ctx->id % VHOST_RX_CORES]->mempool = t->pool;

    /* initialize tx queue */
    t->queue_id = ctx->id;
    rte_spinlock_lock(&initlock);
    ret = rte_eth_tx_queue_setup(global->eth_port_id, t->queue_id, TX_DESCRIPTORS, rte_socket_id(),
                                 &eth_devinfo.default_txconf);
    rte_spinlock_unlock(&initlock);
    if (ret != 0) {
        fprintf(stderr, "network_thread_init: rte_eth_tx_queue_setup failed\n");
        goto error_tx_queue;
    }

    /* barrier to make sure tx queues are initialized first */
    __sync_add_and_fetch(&tx_init_done, 1);
    while (tx_init_done < global->eth_rx_cores)
        ;

    /* initialize rx queue */
    t->queue_id = ctx->id;
    rte_spinlock_lock(&initlock);
    ret = rte_eth_rx_queue_setup(global->eth_port_id, t->queue_id, RX_DESCRIPTORS, rte_socket_id(),
                                 &eth_devinfo.default_rxconf, t->pool);
    rte_spinlock_unlock(&initlock);
    if (ret != 0) {
        fprintf(stderr, "network_thread_init: rte_eth_rx_queue_setup failed\n");
        goto error_rx_queue;
    }

    /* barrier to make sure rx queues are initialized first */
    __sync_add_and_fetch(&rx_init_done, 1);
    while (rx_init_done < global->eth_rx_cores)
        ;

    LOG_IMPT("[%d] NIC TX/RX queue %d\n", ctx->id, t->queue_id);

    /* start device if this ìs core 0 */
    if (ctx->id == 0) {
        if (rte_eth_dev_start(global->eth_port_id) != 0) {
            fprintf(stderr, "rte_eth_dev_start failed\n");
            goto error_tx_queue;
        }

        /* Check and wait for link to be up */
        struct rte_eth_link link;
        int link_check_retries = 10;
        int link_up = 0;
        while (link_check_retries-- > 0) {
            rte_eth_link_get(global->eth_port_id, &link);
            if (link.link_status == RTE_ETH_LINK_UP) {
                link_up = 1;
                fprintf(stderr, "Link is UP: speed=%u Mbps, duplex=%s\n", link.link_speed,
                        link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half");
                break;
            }
            fprintf(stderr, "Waiting for link to come up... (retries left: %d)\n", link_check_retries);
            rte_delay_ms(500);
        }
        if (!link_up) {
            fprintf(stderr, "WARNING: Link is DOWN after starting device. Packets may not transmit!\n");
        }

        /* Enable promiscuous mode to receive all packets (needed for ARP replies and forwarding) */
        if (rte_eth_promiscuous_enable(global->eth_port_id) != 0) {
            fprintf(stderr, "WARNING: Failed to enable promiscuous mode\n");
        } else {
            fprintf(stderr, "Promiscuous mode enabled for port %d\n", global->eth_port_id);
        }

        /* enable vlan stripping if configured */
        if (config.fp_vlan_strip) {
            ret = rte_eth_dev_get_vlan_offload(global->eth_port_id);
            ret |= RTE_ETH_VLAN_STRIP_OFFLOAD;
            if (rte_eth_dev_set_vlan_offload(global->eth_port_id, ret)) {
                fprintf(stderr, "network_thread_init: vlan off set failed\n");
                goto error_tx_queue;
            }
        }

        /* setting up RETA - non-fatal if not supported (e.g., safe mode) */
        if (config.fp_autoscale) {
            if (reta_setup() != 0) {
                fprintf(stderr, "RETA setup failed - continuing without autoscaling support\n");
                /* Don't treat as fatal error - device may not support RSS/RETA */
            }
        }
        start_done = 1;
    }

    /* barrier wait for main thread to start the device */
    while (!start_done)
        ;

    if (config.fp_interrupts) {
        /* setup rx queue interrupt */
        rte_spinlock_lock(&initlock);
        ret =
            rte_eth_dev_rx_intr_ctl_q(global->eth_port_id, t->queue_id, RTE_EPOLL_PER_THREAD, RTE_INTR_EVENT_ADD, NULL);
        rte_spinlock_unlock(&initlock);
        if (ret != 0) {
            fprintf(stderr,
                    "network_thread_init: rte_eth_dev_rx_intr_ctl_q failed "
                    "(%d)\n",
                    rte_errno);
            goto error_int_queue;
        }
    }

    return 0;

error_int_queue:
    /* TODO: destroy rx queue */
error_rx_queue:
    /* TODO: destroy tx queue */
error_tx_queue:
    /* TODO: free mempool */
error_mpool:
    rte_free(t);
    return -1;
}

int network_rx_interrupt_ctl(struct network_thread *t, int turnon) {
    if (turnon) {
        return rte_eth_dev_rx_intr_enable(global->eth_port_id, t->queue_id);
    } else {
        return rte_eth_dev_rx_intr_disable(global->eth_port_id, t->queue_id);
    }
}

// int network_scale_up(uint16_t old, uint16_t new) {
//     uint16_t i, j, k, c, share = rss_reta_size / new;
//     uint16_t outer, inner;

//     /* clear mask */
//     for (k = 0; k < rss_reta_size; k += RTE_RETA_GROUP_SIZE) {
//         rss_reta[k / RTE_RETA_GROUP_SIZE].mask = 0;
//     }

//     k = 0;
//     for (j = old; j < new; j++) {
//         for (i = 0; i < share; i++) {
//             c = core_max(old);

//             for (;; k = (k + 1) % rss_reta_size) {
//                 outer = k / RTE_RETA_GROUP_SIZE;
//                 inner = k % RTE_RETA_GROUP_SIZE;
//                 if (rss_reta[outer].reta[inner] == c) {
//                     rss_reta[outer].mask |= 1ULL << inner;
//                     rss_reta[outer].reta[inner] = j;
//                     fp_state->flow_group_steering[k] = j;
//                     break;
//                 }
//             }

//             rss_core_buckets[c]--;
//             rss_core_buckets[j]++;
//         }
//     }

//     if (rte_eth_dev_rss_reta_update(net_port_id, rss_reta, rss_reta_size) != 0) {
//         fprintf(stderr, "network_scale_up: rte_eth_dev_rss_reta_update failed\n");
//         return -1;
//     }

//     return 0;
// }

// int network_scale_down(uint16_t old, uint16_t new) {
//     uint16_t i, o_c, n_c, outer, inner;

//     /* clear mask */
//     for (i = 0; i < rss_reta_size; i += RTE_RETA_GROUP_SIZE) {
//         rss_reta[i / RTE_RETA_GROUP_SIZE].mask = 0;
//     }

//     for (i = 0; i < rss_reta_size; i++) {
//         outer = i / RTE_RETA_GROUP_SIZE;
//         inner = i % RTE_RETA_GROUP_SIZE;

//         o_c = rss_reta[outer].reta[inner];
//         if (o_c >= new) {
//             n_c = core_min(new);

//             rss_reta[outer].reta[inner] = n_c;
//             rss_reta[outer].mask |= 1ULL << inner;

//             fp_state->flow_group_steering[i] = n_c;

//             rss_core_buckets[o_c]--;
//             rss_core_buckets[n_c]++;
//         }
//     }

//     if (rte_eth_dev_rss_reta_update(net_port_id, rss_reta, rss_reta_size) != 0) {
//         fprintf(stderr, "network_scale_down: rte_eth_dev_rss_reta_update failed\n");
//         return -1;
//     }

//     return 0;
// }

static int reta_setup() {
    uint16_t i, c;

    /* Check if RSS/RETA is supported */
    if (eth_devinfo.reta_size == 0) {
        fprintf(stderr, "reta_setup: RSS/RETA not supported by this device (e.g., Intel ice in safe mode)\n");
        fprintf(stderr, "reta_setup: Continuing without RETA setup - autoscaling will be limited\n");
        rss_reta_size = 0;
        return 0; /* Not an error, just not supported */
    }

    /* allocate RSS redirection table and core-bucket count table */
    rss_reta_size = eth_devinfo.reta_size;
    rss_reta = rte_calloc("rss reta", ((rss_reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) / RTE_ETH_RETA_GROUP_SIZE),
                          sizeof(*rss_reta), 0);
    rss_core_buckets = rte_calloc("rss core buckets", global->fp_cores, sizeof(*rss_core_buckets), 0);

    if (rss_reta == NULL || rss_core_buckets == NULL) {
        fprintf(stderr, "reta_setup: rss_reta alloc failed\n");
        goto error_exit;
    }

    if (rss_reta_size > FLEXNIC_PL_MAX_FLOWGROUPS) {
        fprintf(stderr,
                "reta_setup: reta size (%u) greater than maximum supported"
                " (%u)\n",
                rss_reta_size, FLEXNIC_PL_MAX_FLOWGROUPS);
        abort();
    }

    /* initialize reta */
    for (i = 0, c = 0; i < rss_reta_size; i++) {
        rss_core_buckets[c]++;
        rss_reta[i / RTE_ETH_RETA_GROUP_SIZE].mask = -1ULL;
        rss_reta[i / RTE_ETH_RETA_GROUP_SIZE].reta[i % RTE_ETH_RETA_GROUP_SIZE] = c;
        // fp_state->flow_group_steering[i] = c;
        // c = (c + 1) % fp_cores_cur;
        c = (c + 1) % global->eth_rx_cores;
    }

    if (rte_eth_dev_rss_reta_update(global->eth_port_id, rss_reta, rss_reta_size) != 0) {
        fprintf(stderr, "reta_setup: rte_eth_dev_rss_reta_update failed (RSS/RETA may not be supported)\n");
        fprintf(stderr, "reta_setup: Continuing without RETA setup - autoscaling will be limited\n");
        /* Clean up allocated memory */
        rte_free(rss_core_buckets);
        rte_free(rss_reta);
        rss_reta = NULL;
        rss_core_buckets = NULL;
        rss_reta_size = 0;
        return 0; /* Not fatal - continue without RETA */
    }

    return 0;

error_exit:
    rte_free(rss_core_buckets);
    rte_free(rss_reta);
    rss_reta = NULL;
    rss_core_buckets = NULL;
    rss_reta_size = 0;
    return -1;
}
