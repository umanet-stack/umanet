#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <rte_ring.h>
#include <unistd.h>

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

void vhost_tx_loop(struct vhost_tx_ctx *ctx) {
    LOG_IMPT("[%u] Entering vhost_tx loop...\n", ctx->core_id);

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        // Use vhost_rx_plan for now (TX cores iterate over same devices as RX cores)
        struct vhost_rx_plan *plan = atomic_load(&vhost_rx_plans[ctx->vhost_tx_core_id]);
        for (int i = 0; i < plan->num; i++) {
            uint16_t vid = plan->vids[i];
            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];
            // struct vhost_dev *vdev = vdev_list->vdevs[plan->vids[i]];

            int deq_num = rte_ring_dequeue_burst(global->vhost_tx_rings[plan->vids[i]], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(ctx->vdev_stats[plan->vids[i]], ring_deq_max_count, 1);
            }
            LOG_INFO("[%d] Dequeued %d packets from vhost_tx_ring[%d] to vhost_tx_loop\n", ctx->core_id, deq_num, vid);

            if (deq_num > 0)
                vhost_send(ctx, deq_num, vid, pkts);
        }
    }
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    int16_t ret = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    if (ret == 0) {
        STATS_ADD(ctx->vdev_stats[vid], send_fail_count, 1);
        return 0;
    }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    if (ret == num) {
        STATS_ADD(ctx->vdev_stats[vid], max_send_count, 1);
    }

    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_OUT);

    return ret;
}