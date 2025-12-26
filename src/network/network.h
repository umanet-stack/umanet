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

#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

#include "../include/fastpath.h"
#include "log.h"

struct network_buf_handle;

extern uint8_t net_port_id;
extern uint16_t rss_reta_size;

int network_thread_init(struct dataplane_context *ctx);
int network_rx_interrupt_ctl(struct network_thread *t, int turnon);

static inline void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}

static inline int network_poll(struct dataplane_context *ctx, unsigned num, struct rte_mbuf **pkts) {
    num = rte_eth_rx_burst(net_port_id, ctx->net.queue_id, pkts, num);
    if (num == 0)
        return 0;

    STATS_ADD(ctx, pkt_eth_rx, num);
    STATS_ADD(ctx, call_eth_rx, 1);
    LOG_ETH_IN("[%d] Received %d packets from physical NIC\n", ctx->id, num);
    PRINT_PKTS(pkts, num, LOG_ETH_IN);

    return num;
}

static inline int network_send(struct dataplane_context *ctx, unsigned num, struct rte_mbuf **pkts) {
    uint16_t queued = rte_eth_tx_burst(net_port_id, ctx->net.queue_id, pkts, num);
    if (queued == 0) {
        // TX queue might be full - this could indicate transmission issues
        LOG_WARN("[%d] TX queue full: 0/%u packets queued\n", ctx->id, num);
        return 0;
    }

    if (queued < num) {
        LOG_WARN("[%d] TX queue partial: %u/%u packets queued\n", ctx->id, queued, num);
        STATS_ADD(ctx, eth_tx_partial, 1); // Track partial sends
    }

    STATS_ADD(ctx, pkt_eth_tx, queued);
    STATS_ADD(ctx, call_eth_tx, 1);
    LOG_ETH_OUT("[%d] Sent %d packets to physical NIC\n", ctx->id, queued);
    PRINT_PKTS(pkts, queued, LOG_ETH_OUT);

    return queued;
}

#ifdef FLEXNIC_TRACE_TX
unsigned i;
for (i = 0; i < num; i++) {
    trace_event(FLEXNIC_TRACE_EV_RXPKT, network_buf_len(bhs[i]), network_buf_bufoff(bhs[i]));
}
#endif

#endif /* ndef NETWORK_H_ */
