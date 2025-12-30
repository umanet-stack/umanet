#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/slow/slowpath.h"
#include <rte_ring.h>
#include <unistd.h>

static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

static inline int dst_is_local_subnet(struct rte_mbuf *m) {
    // if (eth_hdr->dst_addr.addr_bytes[0] == 0x12 && eth_hdr->dst_addr.addr_bytes[1] == 0x34 &&
    //     eth_hdr->dst_addr.addr_bytes[2] == 0x56 && eth_hdr->dst_addr.addr_bytes[3] == 0x78 &&
    //     eth_hdr->dst_addr.addr_bytes[4] == 0x90) {
    //     return 1;
    // }
    return 0;
}

void vhost_rx_loop(struct vhost_rx_ctx *ctx) {
    LOG_IMPT("[%u] Entering vhost_rx loop...\n", ctx->core_id);

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        struct vhost_rx_plan *plan = atomic_load(&vhost_rx_plans[ctx->vhost_rx_core_id]);
        for (int i = 0; i < plan->num; i++) {
            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];
            struct vhost_dev *vdev = vdev_list->vdevs[plan->vids[i]];

            int poll_num = vhost_poll(ctx, num, plan->vids[i], pkts);

            struct rte_mbuf *eth_pkts[num];
            struct slow_msg *slow_msgs[num];
            int eth_cnt = 0, slow_cnt = 0;
            // struct {
            //     struct rte_mbuf *pkts[num];
            //     uint16_t cnt;
            // } vm_bucket[MAX_VHOSTS];
            struct rte_mbuf *vm_pkts[MAX_VHOSTS][num];
            uint16_t vm_cnt[MAX_VHOSTS] = {0};
            uint16_t dst_vids[MAX_VHOSTS] = {0};
            uint16_t dst_cnt = 0;

            if (unlikely(vdev->ready == DEVICE_MAC_LEARNING)) {
                struct slow_msg *slow_msg = (struct slow_msg *)malloc(sizeof(struct slow_msg));
                slow_msg->reason = SLOW_MAC_LEARNING;
                slow_msg->src = SLOW_SRC_VHOST;
                slow_msg->vid = plan->vids[i];
                slow_msg->mbuf = pkts[0];
                slow_msgs[slow_cnt++] = slow_msg;
            }

            for (int j = 0; j < poll_num; j++) {
                struct rte_mbuf *m = pkts[j];
                struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

                if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
                    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                    if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
                        struct slow_msg *slow_msg = (struct slow_msg *)malloc(sizeof(struct slow_msg));
                        slow_msg->reason = SLOW_ARP_REQ;
                        slow_msg->src = SLOW_SRC_VHOST;
                        slow_msg->vid = plan->vids[i];
                        slow_msg->mbuf = m;
                        slow_msgs[slow_cnt++] = slow_msg;
                    }
                    // ARP response: forward to vhost/eth
                }

                if (dst_is_local_subnet(m)) {
                    // use ip to vid table
                    // vm_pkts[vm_cnt++] = m;
                    // dst_vids[dst_cnt++] = ...;
                } else {
                    eth_pkts[eth_cnt++] = m;
                }
            }

            if (eth_cnt) {
                int enq_num = rte_ring_enqueue_burst(global->eth_tx_rings[ctx->vhost_rx_core_id], (void **)eth_pkts,
                                                     eth_cnt, NULL);
                LOG_INFO("[%d](%d) enqueued %d packets to eth_tx_ring[%d]\n", ctx->core_id, plan->vids[i], enq_num,
                         ctx->vhost_rx_core_id);
                if (enq_num < eth_cnt) {
                    STATS_ADD(ctx->vdev_stats[ctx->vhost_rx_core_id], ring_enq_fail_count, eth_cnt - enq_num);
                }
            }

            for (int j = 0; j < MAX_VHOSTS; j++) {
                if (vm_cnt[dst_vids[j]] == 0)
                    break;

                int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[dst_vids[j]], (void **)vm_pkts[dst_vids[j]],
                                                     vm_cnt[dst_vids[j]], NULL);
                LOG_INFO("[%d](%d) enqueued %d packets to vhost_tx_ring[%d]\n", ctx->core_id, plan->vids[i], enq_num,
                         dst_vids[j]);
                if (enq_num < vm_cnt[j]) {
                    STATS_ADD(ctx->vdev_stats[dst_vids[j]], ring_enq_fail_count, vm_cnt[j] - enq_num);
                }
            }

            if (slow_cnt) {
                int enq_num = rte_ring_enqueue_burst(global->slowpath_ring, (void **)slow_msgs, slow_cnt, NULL);
                LOG_INFO("[%d](%d) enqueued %d packets to slowpath_ring\n", ctx->core_id, plan->vids[i], enq_num);
                if (enq_num < slow_cnt) {
                    STATS_ADD(ctx->vdev_stats[ctx->vhost_rx_core_id], ring_enq_fail_count, slow_cnt - enq_num);
                }
            }

            int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[plan->vids[i]], (void **)pkts, poll_num, NULL);
            if (enq_num < num) {
                STATS_ADD(ctx->vdev_stats[plan->vids[i]], ring_enq_fail_count, num - enq_num);
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
        return 0;
    }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    if (ret == num) {
        STATS_ADD(ctx->vdev_stats[vid], max_poll_count, 1);
    }

    LOG_VM_IN("[%d](%d) Received %d packets from VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_IN);

    return ret;
}