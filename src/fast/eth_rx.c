#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/slow/slowpath.h"
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <stdint.h>
#include <unistd.h>

static inline unsigned network_poll(struct eth_rx_ctx *ctx, unsigned num, struct rte_mbuf **pkts);

// returns dst_ip if dst_ip is in the local subnet, else 0
static inline int dst_is_local_subnet(struct rte_ether_hdr *eth_hdr) {
    uint32_t subnet = (config.ip & 0xFFFFFF00);
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        uint32_t dst_ip = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
        if ((dst_ip & 0xFFFFFF00) == subnet)
            return dst_ip;
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
        uint32_t dst_ip = rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip);
        if ((dst_ip & 0xFFFFFF00) == subnet)
            return dst_ip;
    }

    return 0;
}

void eth_rx_loop(struct eth_rx_ctx *ctx) {
    LOG_IMPT("[%u] Entering eth_rx loop...\n", ctx->core_id);

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        uint16_t num = MAX_PKT_BURST;
        struct rte_mbuf *pkts[num];
        int poll_num = network_poll(ctx, num, pkts);

        struct rte_mbuf *eth_pkts[num];
        struct slow_msg *slow_msgs[num];
        int eth_cnt = 0, slow_cnt = 0;

        struct {
            struct rte_mbuf *pkts[MAX_PKT_BURST];
            uint16_t cnt;
        } vm_bucket[MAX_VHOSTS] = {0};
        uint8_t vid_seen[MAX_VHOSTS] = {0};
        uint16_t dst_vids[MAX_VHOSTS] = {0};
        uint16_t dst_cnt = 0;

        if (unlikely(vdev->ready == DEVICE_MAC_LEARNING && poll_num > 0)) {
            struct slow_msg *slow_msg = (struct slow_msg *)malloc(sizeof(struct slow_msg));
            slow_msg->reason = SLOW_MAC_LEARNING;
            slow_msg->src = SLOW_SRC_VHOST;
            slow_msg->vid = vid;
            slow_msg->mbuf = pkts[0];
            slow_msgs[slow_cnt++] = slow_msg;
        }

        for (int j = 0; j < poll_num; j++) {
            struct rte_mbuf *m = pkts[j];
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

            if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
                struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                // only ARP req for dataplance, VM ARPs go stright to vhost_tx_loop
                if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
                    rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip) == config.ip) {
                    struct slow_msg *slow_msg = (struct slow_msg *)malloc(sizeof(struct slow_msg));
                    slow_msg->reason = SLOW_ARP_REQ;
                    slow_msg->src = SLOW_SRC_VHOST;
                    slow_msg->vid = vid;
                    slow_msg->mbuf = m;
                    slow_msgs[slow_cnt++] = slow_msg;
                    continue;
                }
                // ARP response: forward to vhost/eth
            }

            int local_dst_ip = dst_is_local_subnet(eth_hdr);
            if (local_dst_ip) {
                // dpdk's ip (192.168.100.1) and ips not belonging to any vms (e.g. 192.168.100.99)
                // won't be found in ip_2_vid table
                int dst_vid = find_vid_by_ip(local_dst_ip);
                if (dst_vid < 0 || dst_vid >= MAX_VHOSTS) {
                    // invalid dst_vid or out of bounds
                    continue;
                }

                vm_bucket[dst_vid].pkts[vm_bucket[dst_vid].cnt++] = m;
                if (vid_seen[dst_vid])
                    continue;

                vid_seen[dst_vid] = 1;
                if (dst_cnt >= MAX_VHOSTS) {
                    LOG_ERROR("[%d] dst_vids array full, dropping vid %d\n", ctx->core_id, dst_vid);
                    continue;
                }
                dst_vids[dst_cnt++] = dst_vid;

            } else {
                eth_pkts[eth_cnt++] = m;
            }
        }

        if (eth_cnt) {
            // rx core i sends to eth tx core i
            int enq_num =
                rte_ring_enqueue_burst(global->eth_tx_rings[ctx->vhost_rx_core_id], (void **)eth_pkts, eth_cnt, NULL);
            LOG_INFO("[%d](%d) enqueued %d packets to eth_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                     ctx->vhost_rx_core_id);
            if (enq_num < eth_cnt) {
                STATS_ADD(ctx->vdev_stats[ctx->vhost_rx_core_id], ring_enq_fail_count, eth_cnt - enq_num);
            }
        }

        for (int j = 0; j < dst_cnt; j++) {
            if (vm_bucket[dst_vids[j]].cnt == 0)
                break;

            int enq_num =
                rte_ring_enqueue_burst(global->vhost_tx_rings[dst_vids[j]], (void **)vm_bucket[dst_vids[j]].pkts,
                                       vm_bucket[dst_vids[j]].cnt, NULL);
            LOG_INFO("[%d](%d) enqueued %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid, enq_num, dst_vids[j]);
            if (enq_num < vm_bucket[dst_vids[j]].cnt) {
                STATS_ADD(ctx->vdev_stats[dst_vids[j]], ring_enq_fail_count, vm_bucket[dst_vids[j]].cnt - enq_num);
            }
        }

        if (slow_cnt) {
            int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs, slow_cnt, NULL);
            LOG_INFO("[%d](%d) enqueued %d packets to slowpath_ring\n", ctx->core_id, vid, enq_num);
            if (enq_num < slow_cnt) {
                STATS_ADD(ctx->vdev_stats[ctx->vhost_rx_core_id], ring_enq_fail_count, slow_cnt - enq_num);
            }
        }
    }
}

static inline unsigned network_poll(struct eth_rx_ctx *ctx, unsigned num, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->stats, call_count, 1);
    int16_t ret = rte_eth_rx_burst(global->eth_port_id, ctx->eth_queue_id, pkts, num);
    if (ret == 0) {
        STATS_ADD(ctx->stats, empty_poll_count, 1);
        return 0;
    }

    STATS_ADD(ctx->stats, pkt_count, ret);
    if (ret == num) {
        STATS_ADD(ctx->stats, max_poll_count, 1);
    }

    LOG_ETH_IN("[%d] Received %d packets from physical NIC\n", ctx->core_id, ret);
    PRINT_PKTS(pkts, ret, LOG_ETH_IN);

    return ret;
}
