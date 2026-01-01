#include "log.h"
#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <unistd.h>

static inline int network_send(struct eth_tx_ctx *ctx, unsigned num, struct rte_mbuf **pkts);

void eth_tx_loop(struct eth_tx_ctx *ctx) {
    LOG_IMPT("[%u] Entering eth_tx loop...\n", ctx->core_id);

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        uint16_t num = MAX_PKT_BURST;
        struct rte_mbuf *pkts[num];

        int deq_num = rte_ring_dequeue_burst(global->eth_tx_rings[ctx->eth_queue_id], (void **)pkts, num, NULL);
        if (deq_num == num) {
            STATS_ADD(ctx->stats, ring_deq_max_count, 1);
        }
        // LOG_INFO("[%d] Dequeued %d packets from eth_tx_ring[%d] to eth_tx_loop\n", ctx->core_id, deq_num,
        //  ctx->eth_queue_id);

        // VMs sent to dataplane MAC 02:00:00:00:00:fe, we forward to physical gateway
        struct rte_ether_hdr *eth_hdr;

        for (int i = 0; i < deq_num; i++) {
            eth_hdr = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
            rte_ether_addr_copy(&global->eth_addr, &eth_hdr->src_addr); // Src: NIC's MAC

            // Preserve broadcast/multicast MACs (for ARP requests, etc.)
            if (rte_is_broadcast_ether_addr(&eth_hdr->dst_addr) || rte_is_multicast_ether_addr(&eth_hdr->dst_addr)) {
                // Keep broadcast/multicast - don't change
            } else if (rte_is_same_ether_addr(&eth_hdr->dst_addr, &config.mac)) {
                // VM sent to other node NIC's MAC
                rte_ether_addr_copy(&config.other_node_mac, &eth_hdr->dst_addr);
            }
            // Otherwise, keep the original destination MAC (for direct communication)
        }

        if (deq_num > 0)
            network_send(ctx, deq_num, pkts);
    }
}

static inline int network_send(struct eth_tx_ctx *ctx, unsigned num, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->stats, call_count, 1);
    int16_t ret = rte_eth_tx_burst(global->eth_port_id, ctx->eth_queue_id, pkts, num);
    if (ret == 0) {
        STATS_ADD(ctx->stats, send_fail_count, 1);
        return 0;
    }

    STATS_ADD(ctx->stats, pkt_count, ret);
    if (ret == num) {
        STATS_ADD(ctx->stats, max_send_count, 1);
    }

    LOG_ETH_OUT("[%d] Sent %d packets to ETH queue %d\n", ctx->core_id, ret, ctx->eth_queue_id);
    PRINT_PKTS(pkts, ret, LOG_ETH_OUT);

    return ret;
}