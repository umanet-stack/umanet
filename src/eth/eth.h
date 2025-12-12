/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef _ETH_H_
#define _ETH_H_

#include <rte_ether.h>
#include <stdint.h>

typedef struct {
    const uint16_t vlan_tags[64];
} eth_state_t;

extern eth_state_t eth;

#endif