/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef _ETH_H_
#define _ETH_H_

#include <rte_ether.h>
#include <stdint.h>

typedef struct {
    /* number of devices/queues to support*/
    uint32_t num_queues;
    uint32_t num_devices;

    struct rte_mempool *mbuf_pool;

    uint16_t num_pf_queues;
    uint16_t vmdq_queue_base;
    uint16_t queues_per_pool;

    const uint16_t vlan_tags[64];

    /* ethernet addresses of ports */
    struct rte_ether_addr vmdq_ports_eth_addr[64];
} eth_state_t;

extern eth_state_t eth;
int port_init(uint16_t port);
unsigned check_ports_num(unsigned nb_ports);

#endif