#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/slow/slowpath.h"
#include <rte_ethdev.h>
#include <rte_gro.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <stdint.h>
#include <unistd.h>

static inline unsigned network_poll(struct eth_rx_ctx *ctx, int rx_queue_id, unsigned num, struct rte_mbuf **out_pkts);

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

    struct rte_mbuf *pkts[MAX_PKT_BURST];
    struct rte_mbuf *vm_pkts[MAX_VHOSTS][MAX_PKT_BURST];
    uint16_t vm_cnt[MAX_VHOSTS];
    for (int i = 0; i < MAX_VHOSTS; i++) {
        vm_cnt[i] = 0;
    }
    uint16_t dst_vids[MAX_VHOSTS]; // indexed by dst_cnt, no need to init

    struct slow_msg *slow_msgs[MAX_PKT_BURST];
    int slow_cnt, poll_num;

    while (1) {
        // STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        for (int i = ctx->eth_rx_queue_r; i < config.eth_rx_queues; i += config.eth_rx_cores) {
            poll_num = network_poll(ctx, i, MAX_PKT_BURST, pkts);
            if (poll_num == 0)
                continue;

            // Prefetch first packets
            for (int p = 0; p < RTE_MIN(poll_num, 4); p++) {
                rte_prefetch0(pkts[p]);
                rte_prefetch0(rte_pktmbuf_mtod(pkts[p], void *));
            }

            slow_cnt = 0;
            uint64_t vid_seen_mask = 0;
            uint16_t dst_cnt = 0;

            struct vdev_list *vdev_list_ptr = atomic_load_explicit(&vdev_list, memory_order_relaxed);
            for (int j = 0; j < poll_num; j++) {
                // Prefetch next packet's mbuf and packet data
                if (j + 1 < poll_num) {
                    rte_prefetch0(pkts[j + 1]);
                    rte_prefetch0(rte_pktmbuf_mtod(pkts[j + 1], void *));
                }

                struct rte_mbuf *m = pkts[j];
                struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

                if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
                    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                    // only ARP req for dataplane, VM ARPs go stright to vhost_tx_loop
                    if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
                        rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip) == config.ip) {
                        struct slow_msg *slow_msg;
                        rte_mempool_get(control_ctx->msg_pool, (void **)&slow_msg);
                        slow_msg->reason = SLOW_ARP_REQ;
                        slow_msg->src = SLOW_SRC_ETH;
                        slow_msg->eth_queue_id = i;
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

                    struct vhost_dev *vdev = vdev_list_ptr->vdevs[dst_vid];
                    rte_ether_addr_copy(&vdev->mac, &eth_hdr->dst_addr);  // dst MAC = vm MAC
                    rte_ether_addr_copy(&config.mac, &eth_hdr->src_addr); // src MAC = our MAC

                    // DEBUG: pkt ol_flags=0x40001a IP_GOOD=0 L4_GOOD=0 nb_segs=1
                    // static int debug_count = 0;
                    // if (debug_count < 5) {
                    //     printf("[eth_rx] pkt ol_flags=0x%lx IP_GOOD=%d L4_GOOD=%d nb_segs=%u\n", m->ol_flags,
                    //            !!(m->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_GOOD),
                    //            !!(m->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_GOOD), m->nb_segs);
                    //     debug_count++;
                    // }

                    // NIC to VM path: clear offload flags and recalculate checksums
                    // Packets from NIC may have pseudo-checksums from sender's TX offload
                    // Virtio requires valid checksums in packet data, not offloaded
                    fix_cksum(m);

                    vm_pkts[dst_vid][vm_cnt[dst_vid]++] = m;
                    if (vid_seen_mask & (1ULL << dst_vid))
                        continue;

                    vid_seen_mask |= (1ULL << dst_vid);
                    if (dst_cnt >= MAX_VHOSTS) {
                        LOG_ERROR("[%d] dst_vids array full, dropping vid %d\n", ctx->core_id, dst_vid);
                        continue;
                    }
                    dst_vids[dst_cnt++] = dst_vid;
                }
            }

            for (int j = 0; j < dst_cnt; j++) {
                if (vm_cnt[dst_vids[j]] == 0)
                    break;

                int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[dst_vids[j]], (void **)vm_pkts[dst_vids[j]],
                                                     vm_cnt[dst_vids[j]], NULL);
                // LOG_INFO("[%d] enqueued %d packets to vhost_tx_ring[%d]\n", ctx->core_id, enq_num, dst_vids[j]);
                if (enq_num < vm_cnt[dst_vids[j]]) {
                    STATS_ADD(ctx->stats, ring_enq_fail_count, vm_cnt[dst_vids[j]] - enq_num);
                }
                vm_cnt[dst_vids[j]] = 0; // reset for next iteration
            }

            if (slow_cnt) {
                int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs, slow_cnt, NULL);
                // LOG_INFO("[%d] enqueued %d packets to slowpath_ring\n", ctx->core_id, enq_num);
                if (enq_num < slow_cnt) {
                    STATS_ADD(ctx->stats, ring_enq_fail_count, slow_cnt - enq_num);
                }
            }
        }
    }
}

static inline unsigned network_poll(struct eth_rx_ctx *ctx, int rx_queue_id, unsigned num, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->stats, call_count, 1);
    int16_t nb_rx = rte_eth_rx_burst(global->eth_port_id, rx_queue_id, pkts, num);
    if (nb_rx == 0) {
        STATS_ADD(ctx->stats, empty_poll_count, 1);
        return 0;
    }

    // DEBUG: Show what NIC gave us BEFORE GRO
    // if (nb_rx > 0) {
    //     printf("NIC RX: got %d packets from burst\n", nb_rx);
    // }

    // GRO disabled: it merges packets but invalidates checksums
    // Virtio VMs require valid checksums in packet data, not offloaded
    // TODO: Re-enable GRO selectively for packets going back to NIC (not to VMs)
    // pkts_set_gro_flags(pkts, nb_rx);
    // uint16_t gro_cnt = rte_gro_reassemble_burst(pkts, nb_rx, &ctx->gro_param);
    // STATS_ADD(ctx->stats, pkt_count, gro_cnt);

    STATS_ADD(ctx->stats, pkt_count, nb_rx);

    // DEBUG: Show what GRO returned
    static int count = 0;
    if (count < 50) {
        // printf("GRO: %d packets in -> %d packets out\n", nb_rx, gro_cnt);
        // for (int i = 0; i < gro_cnt; i++)
        //     printf("  pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
        // count++;
        for (int i = 0; i < nb_rx; i++) {
            if ((pkts[i]->ol_flags & RTE_MBUF_F_TX_TCP_SEG) && pkts[i]->pkt_len <= PKT_MTU) {
                LOG_ERROR("Invalid TSO packet: pkt_len=%u mtu=%u\n", pkts[i]->pkt_len, PKT_MTU);
            }
            LOG_IMPT("ETH RX: pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
        }
        count++;
    }

    LOG_ETH_IN("[%d] Received %d packets from physical NIC RX queue %d\n", ctx->core_id, nb_rx, rx_queue_id);
    PRINT_PKTS(pkts, nb_rx, LOG_ETH_IN);

    return nb_rx;
}
