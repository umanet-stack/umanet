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

#ifndef INTERNAL_H_
#define INTERNAL_H_

#include "src/include/fastpath.h"
#include <stddef.h>
#include <stdint.h>

#include <rte_config.h>
#include <rte_ether.h>

extern struct rte_hash *mac_flow_table;

extern int exited;
extern unsigned fp_cores_max;
extern volatile unsigned fp_cores_cur;
extern volatile unsigned fp_scale_to;

void *util_create_shmsiszed(const char *name, size_t size, void *addr);

enum arp_src { ARP_SRC_VM, ARP_SRC_ETH };
int process_arp(struct dataplane_context *ctx, struct vhost_dev *vdev, struct rte_mbuf *m, enum arp_src src);

uint16_t fastpath_from_vhost(struct dataplane_context *ctx, uint32_t current_device_num);

void flush_eth_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q);
void fastpath_from_eth(struct dataplane_context *ctx);

struct rte_flow *install_mac_flow(uint16_t port_id, const struct rte_ether_addr *mac, uint16_t queue_id);

#endif /* ndef INTERNAL_H_ */
