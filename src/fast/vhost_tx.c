#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/network/network.h"
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

        struct vhost_plan *plan = atomic_load(&vhost_tx_plans[ctx->vhost_tx_core_id]);
        for (int i = 0; i < plan->num; i++) {
            uint16_t vid = plan->vids[i];
            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];

            int deq_num = rte_ring_dequeue_burst(global->vhost_tx_rings[plan->vids[i]], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(ctx->vdev_stats[plan->vids[i]], ring_deq_max_count, 1);
            }
            // LOG_INFO("[%d] Dequeued %d packets from vhost_tx_ring[%d] to vhost_tx_loop\n", ctx->core_id, deq_num,
            // vid);

            if (deq_num > 0)
                vhost_send(ctx, deq_num, vid, pkts);
        }
    }
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    int16_t ret = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    if (ret < 0)
        ret = 0;

    if (ret < num) {
        // pkts[0 .. ret-1]     -> consumed by vhost (free)
        // pkts[ret .. num-1]   -> STILL OWNED BY YOU -> send back to ring (do not free)
        int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[vid], (void **)(pkts + ret), num - ret, NULL);
        if (enq_num < num - ret) {
            LOG_WARN("[%d](%d) failed to requeue %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid,
                     num - ret - enq_num, vid);
            free_pkts(pkts + ret + enq_num, num - ret - enq_num);
        }
        // free_pkts(pkts + ret, num - ret); // if no requeue, free packets
        STATS_ADD(ctx->vdev_stats[vid], requeue_count, 1);
    }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    if (ret == MAX_PKT_BURST) {
        STATS_ADD(ctx->vdev_stats[vid], max_send_count, 1);
    }

    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_OUT);
    free_pkts(pkts, ret);

    return ret;
}