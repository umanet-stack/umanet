#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/slow/slowpath.h"
#include <rte_ip.h>
#include <rte_jhash.h>
#include <rte_ring.h>
#include <rte_tcp.h>
#include <rte_thash.h>
#include <rte_udp.h>
#include <stdint.h>
#include <unistd.h>

static const uint8_t default_rss_key[40] = {0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67,
                                            0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
                                            0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30,
                                            0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

// returns dst_ip if dst_ip is in the local subnet, else 0
static inline int dst_is_local_subnet(struct rte_ether_hdr *eth_hdr, uint32_t *src_ip, uint32_t *dst_ip,
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
    return h % config.eth_tx_cores;
    // uint32_t idx = h & (FLOW_TABLE_SIZE - 1);

    // struct flow_entry *entry = &ctx->flow_table[idx];

    // if (likely(entry->key.src_ip == key->src_ip && entry->key.dst_ip == key->dst_ip)) {
    //     entry->last_seen_tsc = now;
    //     return entry->eth_tx_core;
    // }

    // // miss -> slow path
    // return -1;
}

void vhost_rx_loop(struct vhost_rx_ctx *ctx) {
    LOG_IMPT("[%u] Entering vhost_rx loop...\n", ctx->core_id);
    ctx->iteration_counter = 0;

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
        ctx->iteration_counter++;

        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[ctx->vhost_rx_core_id]);
        for (int i = 0; i < plan->num; i++) {
            uint16_t num = MAX_PKT_BURST;
            uint16_t vid = plan->vids[i];
            if (vid >= MAX_VHOSTS) {
                LOG_ERROR("[%d] Invalid vid %d in plan\n", ctx->core_id, vid);
                continue;
            }

            struct rte_mbuf *pkts[num];
            struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);
            if (vdev_list_ptr == NULL || vdev_list_ptr->vdevs[vid] == NULL) {
                LOG_WARN("[%d] vdev_list or vdevs[%d] is NULL\n", ctx->core_id, vid);
                continue;
            }
            struct vhost_dev *vdev = vdev_list_ptr->vdevs[vid];

            // Adaptive polling: Skip iperf servers (even vm_id) some of the time
            // Servers send ACKs/control packets (important for TCP flow control!), clients send bulk data
            // Poll servers every OTHER iteration to balance efficiency with TCP ACK latency
            // Skipping too aggressively (e.g., 7/8) delays ACKs and throttles clients
            // if (vdev->vm_id >= 0 && (vdev->vm_id % 2 == 0)) {
            //     // This is an iperf server (even vm_id: 0,2,4,6,...)
            //     // Skip every other poll (only poll on even iterations)
            //     if ((ctx->iteration_counter & 0x3) != 0) {
            //         continue; // Skip this poll
            //     }
            // }
            // // Clients (odd vm_id: 1,3,5,7,...) are polled every iteration

            int poll_num = vhost_poll(ctx, num, vid, pkts);
            if (poll_num == 0)
                continue;

            struct slow_msg *slow_msgs[num];
            int slow_cnt = 0;

            struct {
                struct rte_mbuf *pkts[MAX_PKT_BURST];
                uint16_t cnt;
            } eth_bucket[MAX_ETH_TX_CORES] = {0};

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
                STATS_ADD(ctx->vdev_stats[vid], byte_wnd[ctx->vdev_stats[vid]->wnd_idx], rte_pktmbuf_pkt_len(pkts[j]));
                struct rte_mbuf *m = pkts[j];
                struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

                if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
                    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                    // only ARP req for dataplane, VM ARPs go stright to vhost_tx_loop
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

                uint32_t src_ip = 0, dst_ip = 0;
                uint16_t src_port = 0, dst_port = 0;
                uint8_t proto = 0;
                if (dst_is_local_subnet(eth_hdr, &src_ip, &dst_ip, &src_port, &dst_port, &proto)) {
                    // dpdk's ip (192.168.100.1) and ips not belonging to any vms (e.g. 192.168.100.99)
                    // won't be found in ip_2_vid table
                    int dst_vid = find_vid_by_ip(dst_ip);
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
                    eth_bucket[eth_tx_core].pkts[eth_bucket[eth_tx_core].cnt++] = m;
                }
            }

            for (int j = 0; j < config.eth_tx_cores; j++) {
                if (eth_bucket[j].cnt == 0)
                    continue;
                int enq_num = rte_ring_enqueue_burst(global->eth_tx_rings[j], (void **)eth_bucket[j].pkts,
                                                     eth_bucket[j].cnt, NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to eth_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                //          ctx->vhost_rx_core_id);
                if (enq_num < eth_bucket[j].cnt) {
                    int dropped = eth_bucket[j].cnt - enq_num;
                    STATS_ADD(ctx->vdev_stats[vid], eth_tx_ring_enq_fail_count, dropped);
                    for (int k = enq_num; k < eth_bucket[j].cnt; k++) {
                        rte_pktmbuf_free(eth_bucket[j].pkts[k]);
                    }
                }
            }

            for (int j = 0; j < dst_cnt; j++) {
                if (vm_bucket[dst_vids[j]].cnt == 0)
                    break;

                int enq_num =
                    rte_ring_enqueue_burst(global->vhost_tx_rings[dst_vids[j]], (void **)vm_bucket[dst_vids[j]].pkts,
                                           vm_bucket[dst_vids[j]].cnt, NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                //  dst_vids[j]);
                if (enq_num < vm_bucket[dst_vids[j]].cnt) {
                    STATS_ADD(ctx->vdev_stats[dst_vids[j]], vhost_tx_ring_enq_fail_count,
                              vm_bucket[dst_vids[j]].cnt - enq_num);
                }
            }

            if (slow_cnt) {
                int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs, slow_cnt, NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to slowpath_ring\n", ctx->core_id, vid, enq_num);
                if (enq_num < slow_cnt) {
                    LOG_WARN("[%d](%d) failed to enqueue %d packets to slowpath_ring\n", ctx->core_id, vid,
                             slow_cnt - enq_num);
                }
            }
        }
    }
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

    LOG_VM_IN("[%d](%d) Received %d packets from VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_IN);

    return ret;
}
