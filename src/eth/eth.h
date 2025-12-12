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

    uint16_t num_pf_queues;
    uint16_t queues_per_pool;

    const uint16_t vlan_tags[64];
} eth_state_t;

extern eth_state_t eth;
unsigned check_ports_num(unsigned nb_ports);

#endif