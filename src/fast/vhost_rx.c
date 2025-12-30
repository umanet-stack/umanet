#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <rte_ring.h>
#include <unistd.h>

static inline unsigned vhost_poll(struct vhost_rx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

void vhost_rx_loop(struct vhost_rx_ctx *ctx) {
    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        struct vhost_rx_plan *plan = atomic_load(&vhost_rx_plans[ctx->vhost_rx_core_id]);
        for (int i = 0; i < plan->num; i++) {
            struct rte_mbuf *pkts[MAX_PKT_BURST];
            uint16_t num = MAX_PKT_BURST;

            vhost_poll(ctx, num, plan->vids[i], pkts);

            int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[plan->vids[i]], (void **)pkts, num, NULL);
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