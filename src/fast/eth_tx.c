#include "log.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <rte_ethdev.h>
#include <rte_ring.h>

static inline int network_send(struct eth_tx_ctx *ctx, unsigned num, struct rte_mbuf **pkts);

void eth_tx_loop(struct eth_tx_ctx *ctx) {
    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        struct rte_mbuf *pkts[MAX_PKT_BURST];
        uint16_t num = MAX_PKT_BURST;

        rte_ring_dequeue_burst(global->eth_tx_rings[ctx->eth_queue_id], (void **)pkts, num, NULL);
        network_send(ctx, num, pkts);
    }
}

static inline int network_send(struct eth_tx_ctx *ctx, unsigned num, struct rte_mbuf **pkts) {
    uint16_t queued = rte_eth_tx_burst(global->eth_port_id, ctx->eth_queue_id, pkts, num);
    if (queued == 0) {
        // TX queue might be full - this could indicate transmission issues
        LOG_WARN("[%d] TX queue full: 0/%u packets queued\n", ctx->id, num);
        return 0;
    }

    if (queued < num) {
        LOG_WARN("[%d] TX queue partial: %u/%u packets queued\n", ctx->id, queued, num);
        // STATS_ADD(ctx, eth_tx_partial, 1); // Track partial sends
    }

    // STATS_ADD(ctx, pkt_eth_tx, queued);
    // STATS_ADD(ctx, call_eth_tx, 1);
    LOG_ETH_OUT("[%d] Sent %d packets to physical NIC\n", ctx->id, queued);
    PRINT_PKTS(pkts, queued, LOG_ETH_OUT);

    return queued;
}