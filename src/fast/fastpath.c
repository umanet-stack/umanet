#include "src/include/fastpath.h"
#include "src/fast/ecn.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/slow/slowpath.h"
#include <rte_ethdev.h>
#include <rte_gro.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <rte_thash.h>
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

static inline int network_send(struct eth_tx_ctx *ctx, int tx_queue_id, unsigned num, struct rte_mbuf **pkts);

static const uint8_t default_rss_key[40] = {0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67,
                                            0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
                                            0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30,
                                            0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

// returns dst_ip if dst_ip is in the local subnet, else 0
static inline int dst_is_local_subnet_(struct rte_ether_hdr *eth_hdr, uint32_t *src_ip, uint32_t *dst_ip,
                                       uint16_t *src_port, uint16_t *dst_port, uint8_t *proto) {
    uint32_t subnet = (config.ip & 0xFFFFFF00);
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        *src_ip = rte_be_to_cpu_32(ip->src_addr);
        *dst_ip = rte_be_to_cpu_32(ip->dst_addr);

        *proto = ip->next_proto_id;
        if (ip->next_proto_id == IPPROTO_TCP) {
            struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + (ip->ihl * 4));
            *src_port = rte_be_to_cpu_16(tcp->src_port);
            *dst_port = rte_be_to_cpu_16(tcp->dst_port);
        } else if (ip->next_proto_id == IPPROTO_UDP) {
            struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + (ip->ihl * 4));
            *src_port = rte_be_to_cpu_16(udp->src_port);
            *dst_port = rte_be_to_cpu_16(udp->dst_port);
        }
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
        *src_ip = rte_be_to_cpu_32(arp_hdr->arp_data.arp_sip);
        *dst_ip = rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip);
    }

    return (*dst_ip & 0xFFFFFF00) == subnet;
}

// static inline int flow_pick_tx(struct vhost_rx_ctx *ctx, struct flow_key *key, uint64_t now) {
static inline int flow_pick_tx(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, uint8_t proto) {
    uint32_t tuple[4];
    tuple[0] = rte_cpu_to_be_32(src_ip);
    tuple[1] = rte_cpu_to_be_32(dst_ip);
    tuple[2] = rte_cpu_to_be_16(src_port) << 16 | rte_cpu_to_be_16(dst_port);
    tuple[3] = rte_cpu_to_be_32(proto);

    uint32_t h = rte_softrss_be(tuple, 4, default_rss_key);
    return h % config.eth_tx_queues;
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);
static inline unsigned vhost_resend(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

void fp_loop(struct fp_ctx *ctx) {
    struct eth_rx_ctx *eth_rx_ctx = ctx->eth_rx_ctx;
    struct eth_tx_ctx *eth_tx_ctx = ctx->eth_tx_ctx;
    struct vhost_rx_ctx *vhost_rx_ctx = ctx->vhost_rx_ctx;
    struct vhost_tx_ctx *vhost_tx_ctx = ctx->vhost_tx_ctx;

    LOG_IMPT("[%u] Entering fp loop...\n", ctx->core_id);

    struct rte_mbuf *pkts[MAX_PKT_BURST];
    struct rte_mbuf *vm_pkts[MAX_VHOSTS][MAX_PKT_BURST];
    uint16_t vm_cnt[MAX_VHOSTS];
    for (int i = 0; i < MAX_VHOSTS; i++) {
        vm_cnt[i] = 0;
    }
    uint16_t dst_vids[MAX_VHOSTS]; // indexed by dst_cnt, no need to init

    struct slow_msg *slow_msgs[MAX_PKT_BURST];
    int slow_cnt, poll_num;

    LOG_IMPT("[%u] Entering vhost_rx loop...\n", ctx->core_id);
    vhost_rx_ctx->iteration_counter = 0;

    struct rte_mbuf *pkts_[MAX_PKT_BURST];
    struct rte_mbuf *vm_pkts_[MAX_VHOSTS][MAX_PKT_BURST];
    uint16_t vm_cnt_[MAX_VHOSTS];
    for (int i = 0; i < MAX_VHOSTS; i++) {
        vm_cnt_[i] = 0;
    }
    uint16_t dst_vids_[MAX_VHOSTS]; // indexed by dst_cnt, no need to init

    struct rte_mbuf *eth_pkts_[MAX_ETH_TX_QUEUES][MAX_PKT_BURST];
    uint16_t eth_cnt_[MAX_ETH_TX_QUEUES];
    for (int i = 0; i < MAX_ETH_TX_QUEUES; i++) {
        eth_cnt_[i] = 0;
    }

    struct slow_msg *slow_msgs_[MAX_PKT_BURST];
    int slow_cnt_, poll_num_;

    while (1) {
        // STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        for (int i = eth_rx_ctx->eth_rx_queue_r; i < config.eth_rx_queues; i += global->fp_cores) {
            poll_num = network_poll(eth_rx_ctx, i, MAX_PKT_BURST, pkts);
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
                    STATS_ADD(eth_rx_ctx->stats, ring_enq_fail_count, vm_cnt[dst_vids[j]] - enq_num);
                }
                vm_cnt[dst_vids[j]] = 0; // reset for next iteration
            }

            if (slow_cnt) {
                int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs, slow_cnt, NULL);
                // LOG_INFO("[%d] enqueued %d packets to slowpath_ring\n", ctx->core_id, enq_num);
                if (enq_num < slow_cnt) {
                    STATS_ADD(eth_rx_ctx->stats, ring_enq_fail_count, slow_cnt - enq_num);
                }
            }
        }
        eth_rx_ctx->iteration_counter++;

        for (int i = eth_tx_ctx->eth_tx_queue_r; i < config.eth_tx_queues; i += global->fp_cores) {
            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];

            int deq_num = rte_ring_dequeue_burst(global->eth_tx_queue_rings[i], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(eth_tx_ctx->stats, ring_deq_max_count, 1);
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
                network_send(eth_tx_ctx, i, deq_num, pkts);
        }

        vhost_rx_ctx->iteration_counter++;

        struct vhost_plan *plan =
            atomic_load_explicit(&vhost_rx_plans[vhost_rx_ctx->vhost_rx_core_id], memory_order_relaxed);
        struct vdev_list *vdev_list_ptr = atomic_load_explicit(&vdev_list, memory_order_relaxed);
        if (vdev_list_ptr == NULL) {
            continue;
        }

        for (int i = 0; i < plan->num; i++) {
            uint16_t vid = plan->vids[i];
            if (vid >= MAX_VHOSTS || vdev_list_ptr->vdevs[vid] == NULL) {
                LOG_WARN("[%d]  Invalid vid %d or vdevs[%d] is NULL\n", ctx->core_id, vid);
                continue;
            }
            // Prefetch next vdev
            if (i + 1 < plan->num) {
                uint16_t next_vid = plan->vids[i + 1];
                if (next_vid < MAX_VHOSTS && vdev_list_ptr->vdevs[next_vid] != NULL) {
                    rte_prefetch0(vdev_list_ptr->vdevs[next_vid]);
                }
            }

            struct vhost_dev *vdev = vdev_list_ptr->vdevs[vid];

            // Adaptive polling: skip idle vms some of the time
            switch (vhost_rx_ctx->vhost_ap[vid].state) {
            case RX_HOT:
                break; // poll always
            case RX_WARM:
                if (vhost_rx_ctx->iteration_counter & 3) // skip 3/4
                    continue;
                break;
            case RX_COOL:
                if (vhost_rx_ctx->iteration_counter & 7) // skip 7/8
                    continue;
                break;
            case RX_COLD:
                if (vhost_rx_ctx->iteration_counter & 15) // skip 15/16
                    continue;
                break;
            case RX_FROZEN:
                if (rte_rdtsc() < vhost_rx_ctx->vhost_ap[vid].blocked_until_tsc)
                    continue;
                vhost_rx_ctx->vhost_ap[vid].idle_score = 8;
                vhost_rx_ctx->vhost_ap[vid].state = RX_COOL; // thaw gradually
            }

            poll_num_ = vhost_poll(vhost_rx_ctx, MAX_PKT_BURST, vid, pkts_);
            update_rx_state(&vhost_rx_ctx->vhost_ap[vid], poll_num_, vhost_rx_ctx->poll_states);
            if (poll_num_ == 0) {
                continue;
            }

            // Prefetch first packets
            // prefetching a small window (like 2–8, commonly 4) gives the CPU time to bring cache lines in before you
            // actually touch them in the hot loop
            for (int p = 0; p < RTE_MIN(poll_num_, 4); p++) {
                rte_prefetch0(pkts_[p]);
                rte_prefetch0(rte_pktmbuf_mtod(pkts_[p], void *));
            }

            slow_cnt_ = 0;
            uint64_t vid_seen_mask = 0;
            uint16_t dst_cnt = 0;

            if (unlikely(vdev->ready == DEVICE_MAC_LEARNING && poll_num_ > 0)) {
                struct slow_msg *slow_msg;
                rte_mempool_get(control_ctx->msg_pool, (void **)&slow_msg);
                slow_msg->reason = SLOW_MAC_LEARNING;
                slow_msg->src = SLOW_SRC_VHOST;
                slow_msg->vid = vid;
                slow_msg->mbuf = pkts_[0];
                slow_msgs_[slow_cnt_++] = slow_msg;
            }

            for (int j = 0; j < poll_num_; j++) {
                // Prefetch mbuf and first 64 bytes of packet (enough for headers)
                if (j + 1 < poll_num_) {
                    rte_prefetch0(pkts_[j + 1]);                           // prefetch next mbuf struct
                    rte_prefetch0(rte_pktmbuf_mtod(pkts_[j + 1], void *)); // prefetch packet data
                }

                // if (unlikely(pkts[j]->pkt_len > 2000))
                //     LOG_IMPT("[%d](%d) GSO pkt: len=%u\n", ctx->core_id, vid, pkts[j]->pkt_len);
                // LOG_IMPT("[%d](%d) ol_flags=0x%lx tso=%u\n", ctx->core_id, vid, pkts[j]->ol_flags,
                // pkts[j]->tso_segsz);

                STATS_ADD(vhost_rx_ctx->vdev_stats[vid], byte_wnd[vhost_rx_ctx->vdev_stats[vid]->wnd_idx],
                          rte_pktmbuf_pkt_len(pkts_[j]));
                struct rte_mbuf *m = pkts_[j];
                struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

                if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
                    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                    // only ARP req for dataplane, VM ARPs go stright to vhost_tx_loop
                    if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST) &&
                        rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip) == config.ip) {
                        struct slow_msg *slow_msg;
                        rte_mempool_get(control_ctx->msg_pool, (void **)&slow_msg);
                        slow_msg->reason = SLOW_ARP_REQ;
                        slow_msg->src = SLOW_SRC_VHOST;
                        slow_msg->vid = vid;
                        slow_msg->mbuf = m;
                        slow_msgs_[slow_cnt_++] = slow_msg;
                        continue;
                    }
                    // ARP response: forward to vhost/eth
                }

                uint32_t src_ip = 0, dst_ip = 0;
                uint16_t src_port = 0, dst_port = 0;
                uint8_t proto = 0;
                if (dst_is_local_subnet_(eth_hdr, &src_ip, &dst_ip, &src_port, &dst_port, &proto)) {
                    // dpdk's ip (192.168.100.1) and ips not belonging to any vms (e.g. 192.168.100.99)
                    // won't be found in ip_2_vid table
                    int dst_vid = find_vid_by_ip(dst_ip);
                    if (dst_vid < 0 || dst_vid >= MAX_VHOSTS) {
                        // invalid dst_vid or out of bounds
                        continue;
                    }

                    vm_pkts_[dst_vid][vm_cnt_[dst_vid]++] = m;
                    if (vid_seen_mask & (1ULL << dst_vid))
                        continue;

                    vid_seen_mask |= (1ULL << dst_vid);
                    if (dst_cnt >= MAX_VHOSTS) {
                        LOG_ERROR("[%d] dst_vids array full, dropping vid %d\n", ctx->core_id, dst_vid);
                        continue;
                    }
                    dst_vids_[dst_cnt++] = dst_vid;

                } else {
                    // struct flow_key eth_key = {
                    //     .src_ip = src_ip,
                    //     .dst_ip = dst_ip,
                    // };
                    // int eth_tx_core = flow_pick_tx(ctx, &eth_key, rte_rdtsc());
                    int eth_tx_core = flow_pick_tx(src_ip, dst_ip, src_port, dst_port, proto);
                    // if (eth_tx_core < 0 || eth_tx_core >= MAX_ETH_TX_CORES) {
                    //     struct slow_msg *slow_msg = (struct slow_msg *)malloc(sizeof(struct slow_msg));
                    //     slow_msg->reason = SLOW_ETH_TX_FLOW;
                    //     slow_msg->src = SLOW_SRC_VHOST;
                    //     slow_msg->mbuf = m;
                    //     slow_msgs[slow_cnt++] = slow_msg;
                    //     continue;
                    // }
                    eth_pkts_[eth_tx_core][eth_cnt_[eth_tx_core]++] = m;
                }
            }

            for (int j = 0; j < config.eth_tx_queues; j++) {
                if (eth_cnt_[j] == 0)
                    continue;

                int congestion = RING_SIZE - rte_ring_free_count(global->eth_tx_queue_rings[j]);
                ecn_mark_packets(eth_pkts_[j], eth_cnt_[j], congestion, &vhost_rx_ctx->ecn_rr_eth[j]);

                // if ring is full, enqueue < n (possibly 0) = if vhost tx slow, vhost rx will be made slow
                int enq_num =
                    rte_ring_enqueue_burst(global->eth_tx_queue_rings[j], (void **)eth_pkts_[j], eth_cnt_[j], NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to eth_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                //          ctx->vhost_rx_core_id);
                if (enq_num < eth_cnt_[j]) {
                    int dropped = eth_cnt_[j] - enq_num;
                    STATS_ADD(vhost_rx_ctx->vdev_stats[vid], eth_tx_ring_enq_fail_count, dropped);
                    for (int k = enq_num; k < eth_cnt_[j]; k++) {
                        rte_pktmbuf_free(eth_pkts_[j][k]);
                    }
                }
                eth_cnt_[j] = 0; // reset for next iteration
            }

            for (int j = 0; j < dst_cnt; j++) {
                if (vm_cnt_[dst_vids_[j]] == 0)
                    break;

                int congestion = RING_SIZE - rte_ring_free_count(global->vhost_tx_rings[dst_vids_[j]]);
                ecn_mark_packets(vm_pkts_[dst_vids_[j]], vm_cnt_[dst_vids_[j]], congestion,
                                 &vhost_rx_ctx->ecn_rr_vhost[dst_vids_[j]]);

                int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[dst_vids_[j]],
                                                     (void **)vm_pkts_[dst_vids_[j]], vm_cnt_[dst_vids_[j]], NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                //  dst_vids[j]);
                if (enq_num < vm_cnt_[dst_vids_[j]]) {
                    STATS_ADD(vhost_rx_ctx->vdev_stats[dst_vids_[j]], vhost_tx_ring_enq_fail_count,
                              vm_cnt_[dst_vids_[j]] - enq_num);
                }
                vm_cnt_[dst_vids_[j]] = 0; // reset for next iteration
            }

            if (slow_cnt_) {
                int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs_, slow_cnt_, NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to slowpath_ring\n", ctx->core_id, vid, enq_num);
                if (enq_num < slow_cnt_) {
                    LOG_WARN("[%d](%d) failed to enqueue %d packets to slowpath_ring\n", ctx->core_id, vid,
                             slow_cnt_ - enq_num);
                }
            }
        }

        struct vhost_plan *plan_ =
            atomic_load_explicit(&vhost_tx_plans[vhost_tx_ctx->vhost_tx_core_id], memory_order_relaxed);
        for (int i = 0; i < plan_->num; i++) {
            uint16_t vid = plan_->vids[i];
            if (vid >= MAX_VHOSTS || vid == (uint16_t)-1) {
                continue;
            }

            if (vhost_tx_ctx->vm_bp[vid].state == VM_BLOCKED_TX &&
                rte_rdtsc() < vhost_tx_ctx->vm_bp[vid].blocked_until_tsc)
                continue;

            if (vhost_tx_ctx->retry_cnts[vid] > 0) {
                vhost_resend(vhost_tx_ctx, vhost_tx_ctx->retry_cnts[vid], vid, vhost_tx_ctx->retry_pkts[vid]);
                continue;
            }

            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];

            int deq_num = rte_ring_dequeue_burst(global->vhost_tx_rings[vid], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(vhost_tx_ctx->vdev_stats[vid], ring_deq_max_count, 1);
            }
            // LOG_INFO("[%d] Dequeued %d packets from vhost_tx_ring[%d] to vhost_tx_loop\n", ctx->core_id, deq_num,
            // vid);

            if (deq_num > 0) {
                for (int j = 0; j < RTE_MIN(deq_num, 4); j++) {
                    rte_prefetch0(pkts[j]);
                }
                vhost_send(vhost_tx_ctx, deq_num, vid, pkts);
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
    STATS_ADD(ctx->stats, pkt_count, nb_rx);

    // if (nb_rx > 0 || (ctx->iteration_counter & 1023) == 0) { // force flush every 1024 iterations
    //     pkts_set_gro_flags(pkts, nb_rx);
    //     // pkts will be left with unassembled pkts, assembled pkts are moved to gro_ctx table
    //     int left_cnt = rte_gro_reassemble(pkts, nb_rx, ctx->gro_ctx);

    //     struct rte_mbuf *flush_pkts[num - left_cnt];
    //     int flush_cnt = 0;
    //     if ((ctx->iteration_counter & 1023) == 0) {
    //         flush_cnt = rte_gro_timeout_flush(ctx->gro_ctx, 0, RTE_GRO_TCP_IPV4, flush_pkts, num - left_cnt);
    //     } else { // flows older than 10us
    //         flush_cnt = rte_gro_timeout_flush(ctx->gro_ctx, 10000, RTE_GRO_TCP_IPV4, flush_pkts, num - left_cnt);
    //     }

    //     static int gro_count = 0;
    //     if (gro_count < 50) {
    //         LOG_IMPT("ETH RX: GRO reassembled %d packets, flushed %d\n", nb_rx - left_cnt, flush_cnt);
    //         gro_count++;
    //     }

    //     for (int i = 0; i < flush_cnt; i++) {
    //         pkts[i + left_cnt] = flush_pkts[i];
    //     }
    //     nb_rx = left_cnt + flush_cnt;
    // }

    static int count = 0;
    if (count < 50) {
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

static inline int network_send(struct eth_tx_ctx *ctx, int tx_queue_id, unsigned num, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->stats, call_count, 1);

    // GRO didnt help compress pkts, maybe try the stateful one later
    // pkts_set_gro_flags(pkts, num);
    // int gro_num = rte_gro_reassemble_burst(pkts, num, &ctx->gro_param);
    // static int gro_count = 0;
    // if (gro_count < 50) {
    //     LOG_IMPT("ETH TX: GRO reassembled %d -> %d packets\n", num, gro_num);
    //     gro_count++;
    // }
    // num = gro_num;

    pkts_set_tso_flags(pkts, num);
    int16_t ret = rte_eth_tx_burst(global->eth_port_id, tx_queue_id, pkts, num);
    if (ret < 0)
        ret = 0;

    static int count = 0;
    if (count < 50) {
        for (int i = 0; i < num; i++) {
            LOG_IMPT("ETH TX: pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
        }
        count++;
    }

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

// copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
// This can fail if the vhost connection is broken
static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    int16_t ret = rte_vhost_dequeue_burst(vid, VIRTIO_TXQ, ctx->mempool, pkts, num);
    if (ret == 0) {
        STATS_ADD(ctx->vdev_stats[vid], empty_poll_count, 1);
        STATS_ADD(ctx->vdev_stats[vid], empty_wnd[ctx->vdev_stats[vid]->wnd_idx], 1);
        return 0;
    }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    STATS_ADD(ctx->vdev_stats[vid], pkt_wnd[ctx->vdev_stats[vid]->wnd_idx], ret);
    if (ret == MAX_PKT_BURST) {
        STATS_ADD(ctx->vdev_stats[vid], max_poll_count, 1);
    }

    // static int count = 0;
    // if (count < 500) {
    //     for (int i = 0; i < ret; i++) {
    //         LOG_IMPT("VHOST RX: pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
    //     }
    //     count++;
    // }

    LOG_VM_IN("[%d](%d) Received %d packets from VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_IN);

    return ret;
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    // pkts_set_tso_flags(pkts, num);
    int16_t ret = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    // CRITICAL: Handle error case (negative return = -1 on error)
    if (ret < 0) {
        ret = 0;
    }

    if (ret < num) {
        // pkts[0 .. ret-1]     -> consumed by vhost (free)
        // pkts[ret .. num-1]   -> STILL OWNED BY YOU -> send back to ring (do not free)
        // int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[vid], (void **)(pkts + ret), num - ret, NULL);
        // if (enq_num < num - ret) {
        //     // LOG_WARN("[%d](%d) failed to requeue %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid,
        //     //          num - ret - enq_num, vid);
        //     free_pkts(pkts + ret + enq_num, num - ret - enq_num);
        // }
        ctx->vm_bp[vid].state = VM_BLOCKED_TX;
        ctx->vm_bp[vid].blocked_until_tsc = rte_rdtsc() + BACKOFF_TSC;
        for (int i = 0; i < num - ret; i++) {
            ctx->retry_pkts[vid][i] = pkts[ret + i];
        }
        ctx->retry_cnts[vid] = num - ret;
    } else {
        // All packets sent successfully - clear backpressure
        ctx->vm_bp[vid].state = VM_ACTIVE;
    }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    if (ret == MAX_PKT_BURST) {
        STATS_ADD(ctx->vdev_stats[vid], max_send_count, 1);
    }

    // static int count = 0;
    // if (count < 500) {
    //     for (int i = 0; i < ret; i++) {
    //         LOG_IMPT("VHOST TX: pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
    //     }
    //     count++;
    // }

    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_OUT);
    free_pkts(pkts, ret);

    return ret;
}

static inline unsigned vhost_resend(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    // for (int i = 0; i < num; i++) {
    //     printf("VHOST TX pkt %d: nb_segs=%u pkt_len=%u\n", i, pkts[i]->nb_segs, pkts[i]->pkt_len);
    // }
    int16_t ret = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    if (ret < 0) {
        ret = 0;
    }

    if (ret == MAX_PKT_BURST) {
        STATS_ADD(ctx->vdev_stats[vid], max_send_count, 1);
    }

    if (ret < num) {
        ctx->vm_bp[vid].state = VM_BLOCKED_TX;
        ctx->vm_bp[vid].blocked_until_tsc = rte_rdtsc() + BACKOFF_TSC;

    } else {
        ctx->vm_bp[vid].state = VM_ACTIVE;
    }

    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_OUT);
    STATS_ADD(ctx->vdev_stats[vid], requeue_pkt_count, ret);
    ctx->retry_cnts[vid] = 0;
    free_pkts(pkts, num);
    return ret;
}