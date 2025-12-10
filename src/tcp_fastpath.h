
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Your Name
 */

#include "tcp_offload.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>

/* Check if packet is TCP and parse headers */
int tcp_parse_packet(struct rte_mbuf *m, uint32_t *sip, uint32_t *dip, uint16_t *sport, uint16_t *dport);
