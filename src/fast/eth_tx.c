#include "log.h"
#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <unistd.h>

static inline int network_send(struct eth_tx_ctx *ctx, int tx_queue_id, unsigned num, struct rte_mbuf **pkts);

void eth_tx_loop(struct eth_tx_ctx *ctx) {
    LOG_IMPT("[%u] Entering eth_tx loop...\n", ctx->core_id);

    while (1) {
        // STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
        for (int i = ctx->eth_tx_queue_r; i < config.eth_tx_queues; i += config.eth_tx_cores) {
            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];

            int deq_num = rte_ring_dequeue_burst(global->eth_tx_queue_rings[i], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(ctx->stats, ring_deq_max_count, 1);
            }
            // LOG_INFO("[%d] Dequeued %d packets from eth_tx_ring[%d] to eth_tx_loop\n", ctx->core_id, deq_num,
            //  ctx->eth_queue_id);

            // VMs sent to dataplane MAC 02:00:00:00:00:fe, we forward to physical gateway
            struct rte_ether_hdr *eth_hdr;

            for (int j = 0; j < deq_num; j++) {
                if (j + 1 < deq_num) {
                    rte_prefetch0(pkts[j + 1]);
                    rte_prefetch0(rte_pktmbuf_mtod(pkts[j + 1], void *));
                }

                eth_hdr = rte_pktmbuf_mtod(pkts[j], struct rte_ether_hdr *);
                rte_ether_addr_copy(&global->eth_addr, &eth_hdr->src_addr); // Src: NIC's MAC

                // Preserve broadcast/multicast MACs (for ARP requests, etc.)
                if (rte_is_broadcast_ether_addr(&eth_hdr->dst_addr) ||
                    rte_is_multicast_ether_addr(&eth_hdr->dst_addr)) {
                    // Keep broadcast/multicast - don't change
                } else if (rte_is_same_ether_addr(&eth_hdr->dst_addr, &config.mac)) {
                    // VM sent to other node NIC's MAC
                    rte_ether_addr_copy(&config.other_node_mac, &eth_hdr->dst_addr);
                }
                // Otherwise, keep the original destination MAC (for direct communication)
            }

            if (deq_num > 0)
                network_send(ctx, i, deq_num, pkts);
        }
    }
}

static inline int network_send(struct eth_tx_ctx *ctx, int tx_queue_id, unsigned num, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->stats, call_count, 1);
    pkts_set_flags(pkts, num);
    int16_t ret = rte_eth_tx_burst(global->eth_port_id, tx_queue_id, pkts, num);
    if (ret < 0)
        ret = 0;

    if (ret < num) {
        // pkts[0 .. ret-1]     -> consumed by NIC (do not free)
        // pkts[ret .. num-1]   -> STILL OWNED BY YOU -> send back to ring (do not free)
        int enq_num =
            rte_ring_enqueue_burst(global->eth_tx_queue_rings[tx_queue_id], (void **)(pkts + ret), num - ret, NULL);
        if (enq_num < num - ret) {
            // LOG_WARN("[%d](%d) failed to requeue %d packets to eth_tx_ring[%d]\n", ctx->core_id, ctx->eth_queue_id,
            //          num - ret - enq_num, ctx->eth_queue_id);
            free_pkts(pkts + ret + enq_num, num - ret - enq_num);
        }
        // free_pkts(pkts + ret, num - ret); // if no requeue, free packets

        // requeue only ONCE
        // int16_t ret2 = rte_eth_tx_burst(global->eth_port_id, ctx->eth_queue_id, pkts + ret, num - ret);
        // if (ret2 < num - ret) {
        // LOG_WARN("[%d](%d) failed to requeue %d packets to ETH queue %d\n", ctx->core_id, ctx->eth_queue_id,
        //          num - ret - ret2, ctx->eth_queue_id);
        //     free_pkts(pkts + ret + ret2, num - ret - ret2);
        // }
        STATS_ADD(ctx->stats, requeue_pkt_count, enq_num);
    }

    STATS_ADD(ctx->stats, pkt_count, ret);
    if (ret == num) {
        STATS_ADD(ctx->stats, max_send_count, 1);
    }

    LOG_ETH_OUT("[%d] Sent %d packets to ETH TX queue %d\n", ctx->core_id, ret, tx_queue_id);
    PRINT_PKTS(pkts, ret, LOG_ETH_OUT);

    return ret;
}