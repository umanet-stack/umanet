#include "src/include/fastpath.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/slow/slowpath.h"
#include <rte_ip.h>
#include <rte_ring.h>
#include <stdint.h>
#include <unistd.h>

static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

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
                int enq_num = rte_ring_enqueue_burst(global->eth_tx_rings[ctx->vhost_rx_core_id], (void **)eth_pkts,
                                                     eth_cnt, NULL);
                // LOG_INFO("[%d](%d) enqueued %d packets to eth_tx_ring[%d]\n", ctx->core_id, vid, enq_num,
                //  ctx->vhost_rx_core_id);
                if (enq_num < eth_cnt) {
                    LOG_WARN("[%d](%d) failed to enqueue %d packets to eth_tx_ring[%d]\n", ctx->core_id, vid,
                             eth_cnt - enq_num, ctx->vhost_rx_core_id);
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
                    STATS_ADD(ctx->vdev_stats[dst_vids[j]], ring_enq_fail_count, vm_bucket[dst_vids[j]].cnt - enq_num);
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
