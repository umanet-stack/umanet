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

#include "../include/main.h"
#include "network.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <utils.h>
#include <utils_rng.h>

// Increased for MTU 9000 - larger packets need more descriptors
#define RX_DESCRIPTORS 2048 // 256 -> 2048 (8x increase)
#define TX_DESCRIPTORS 2048 // 128 -> 2048 (16x increase)

static struct rte_eth_conf port_conf = {
    .rxmode =
        {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
            .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_TCP_CKSUM | RTE_ETH_RX_OFFLOAD_RSS_HASH,
        },
    .txmode =
        {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = RTE_ETH_TX_OFFLOAD_TCP_TSO | RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                        RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        },
    .rx_adv_conf =
        {
            .rss_conf =
                {
                    .rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP,
                },
        },
    .intr_conf =
        {
            .rxq = 0,
        },
};

static struct network_rx_thread **net_threads;

static struct rte_eth_dev_info eth_devinfo;

uint16_t rss_reta_size;
static struct rte_eth_rss_reta_entry64 *rss_reta = NULL;
static uint16_t *rss_core_buckets = NULL;

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

    uint64_t rx_offloads = 0;

    // Check if NIC supports these features
    if (eth_devinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM)
        rx_offloads |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM;
    if (eth_devinfo.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TCP_CKSUM)
        rx_offloads |= RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
    port_conf.rxmode.offloads = rx_offloads;

    // TSO
    eth_devinfo.default_txconf.offloads = port_conf.txmode.offloads;
    eth_devinfo.default_rxconf.offloads = port_conf.rxmode.offloads;

    if (eth_devinfo.max_rx_queues < config.eth_rx_cores || eth_devinfo.max_tx_queues < config.eth_tx_cores) {
        LOG_ERROR("Error: NIC does not support enough hw queues (rx=%u tx=%u)"
                  " for the requested number of cores (%u)\n",
                  eth_devinfo.max_rx_queues, eth_devinfo.max_tx_queues, config.eth_rx_cores);
        goto error_exit;
    }

    /* mask unsupported RSS hash functions */
    if ((port_conf.rx_adv_conf.rss_conf.rss_hf & eth_devinfo.flow_type_rss_offloads) !=
        port_conf.rx_adv_conf.rss_conf.rss_hf) {
        LOG_WARN("NIC does not support all requested RSS hash functions.\n");
        port_conf.rx_adv_conf.rss_conf.rss_hf &= eth_devinfo.flow_type_rss_offloads;
    }

    /* enable per port checksum offload if requested */
    // if (config.fp_xsumoffload) {
    //     uint64_t requested_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
    //     /* mask unsupported TX offloads */
    //     port_conf.txmode.offloads = requested_offloads & eth_devinfo.tx_offload_capa;
    //     if (port_conf.txmode.offloads != requested_offloads) {
    //         LOG_WARN("NIC does not support all requested TX offloads (requested: 0x%lx, supported: 0x%lx, "
    //                  "using: 0x%lx).\n",
    //                  requested_offloads, eth_devinfo.tx_offload_capa, port_conf.txmode.offloads);
    //     }
    // }

    /* disable rx interrupts if requested */
    // if (!config.fp_interrupts)
    //     port_conf.intr_conf.rxq = 0;

    /* initialize port */
    ret = rte_eth_dev_configure(global->eth_port_id, config.eth_rx_queues, config.eth_tx_queues, &port_conf);
    if (ret < 0) {
        LOG_ERROR("rte_eth_dev_configure failed\n");
        goto error_exit;
    }

    if (rte_eth_dev_set_mtu(global->eth_port_id, PKT_MTU) != 0) {
        LOG_ERROR("rte_eth_dev_set_mtu failed\n");
        goto error_exit;
    }

    // eth_devinfo.default_rxconf.offloads = 0;

    /* enable per-queue checksum offload if requested */
    // eth_devinfo.default_txconf.offloads = 0;
    // if (config.fp_xsumoffload) {
    //     uint64_t requested_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
    //     /* mask unsupported TX offloads (use same mask as port-level) */
    //     eth_devinfo.default_txconf.offloads = requested_offloads & eth_devinfo.tx_offload_capa;
    // }

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

static volatile uint32_t tx_init_done = 0;
static volatile uint32_t rx_init_done = 0;
static volatile uint32_t start_done = 0;

int network_tx_queue_init(struct eth_tx_ctx *ctx) {
    int ret;
    for (int i = ctx->eth_tx_queue_r; i < config.eth_tx_queues; i += config.eth_tx_cores) {
        rte_spinlock_lock(&initlock);
        ret = rte_eth_tx_queue_setup(global->eth_port_id, i, TX_DESCRIPTORS, rte_socket_id(),
                                     &eth_devinfo.default_txconf);
        rte_spinlock_unlock(&initlock);
        if (ret != 0) {
            LOG_ERROR("network_tx_queue_init: rte_eth_tx_queue_setup failed\n");
            return -1;
        }

        /* barrier to make sure tx queues are initialized first */
        __sync_add_and_fetch(&tx_init_done, 1);

        LOG_IMPT("[%d] NIC TX queue %d initialized\n", ctx->core_id, i);
    }
    return 0;
}

int network_rx_queue_init(struct eth_rx_ctx *ctx) {
    while (tx_init_done < config.eth_tx_cores)
        ;

    int ret;
    for (int i = ctx->eth_rx_queue_r; i < config.eth_rx_queues; i += config.eth_rx_cores) {
        rte_spinlock_lock(&initlock);
        ret = rte_eth_rx_queue_setup(global->eth_port_id, i, RX_DESCRIPTORS, rte_socket_id(),
                                     &eth_devinfo.default_rxconf, ctx->mempool);
        rte_spinlock_unlock(&initlock);
        if (ret != 0) {
            LOG_ERROR("network_rx_queue_init: rte_eth_rx_queue_setup failed\n");
            return -1;
        }

        /* barrier to make sure rx queues are initialized first */
        __sync_add_and_fetch(&rx_init_done, 1);

        LOG_IMPT("[%d] NIC RX queue %d initialized\n", ctx->core_id, i);
    }
    return 0;
}

int network_start_eth() {
    while (rx_init_done < config.eth_rx_cores)
        ;

    int ret;
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

    start_done = 1;
    return 0;

error_tx_queue:
    return -1;
}
