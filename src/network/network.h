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

#ifndef NETWORK_H_
#define NETWORK_H_

#include "src/include/state.h"
#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

int network_init();
void network_cleanup(void);
void network_dump_stats(void);

static inline void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}

#define PERTHREAD_MBUFS 2048
#define BUFFER_SIZE 2048
#define MBUF_SIZE (BUFFER_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)

struct rte_mempool *network_mempool_alloc() {
    static unsigned pool_id = 0;
    unsigned n;
    char name[32];
    n = __sync_fetch_and_add(&pool_id, 1);
    snprintf(name, 32, "mbuf_pool_%u\n", n);
    return rte_mempool_create(name, PERTHREAD_MBUFS, MBUF_SIZE, 32, sizeof(struct rte_pktmbuf_pool_private),
                              rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL, rte_socket_id(), 0);
}

int network_tx_queue_init(struct eth_tx_ctx *ctx);
int network_rx_queue_init(struct eth_rx_ctx *ctx);
int network_start_eth();

#endif /* ndef NETWORK_H_ */
