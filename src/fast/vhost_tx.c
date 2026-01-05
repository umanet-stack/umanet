#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include <rte_ring.h>
#include <stdatomic.h>
#include <unistd.h>

void vm_bp_init(struct vhost_tx_ctx *ctx) {
    for (int i = 0; i < MAX_VHOSTS; i++) {
        ctx->vm_bp[i].state = VM_ACTIVE;
        ctx->vm_bp[i].blocked_until_tsc = 0;
    }
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

void vhost_tx_loop(struct vhost_tx_ctx *ctx) {
    LOG_IMPT("[%u] Entering vhost_tx loop...\n", ctx->core_id);
    vm_bp_init(ctx);

    while (1) {
        // STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        struct vhost_plan *plan = atomic_load_explicit(&vhost_tx_plans[ctx->vhost_tx_core_id], memory_order_relaxed);
        for (int i = 0; i < plan->num; i++) {
            uint16_t vid = plan->vids[i];
            if (vid >= MAX_VHOSTS || vid == (uint16_t)-1) {
                continue;
            }

            // if (ctx->vm_bp[vid].state == VM_BLOCKED_TX && rte_rdtsc() < ctx->vm_bp[vid].blocked_until_tsc)
            //     continue;

            // if (ctx->retry_cnts[vid] > 0) {
            //     int ret = vhost_send(ctx, ctx->retry_cnts[vid], vid, ctx->retry_pkts[vid]);
            //     if (ret > 0) {
            //         free_pkts(ctx->retry_pkts[vid], ret);
            //         // Shift remaining retry packets to front of array
            //         for (int k = 0; k < ctx->retry_cnts[vid] - ret; k++) {
            //             ctx->retry_pkts[vid][k] = ctx->retry_pkts[vid][k + ret];
            //         }
            //         ctx->retry_cnts[vid] -= ret;
            //     }
            //     STATS_ADD(ctx->vdev_stats[vid], requeue_pkt_count, ret);
            //     continue;
            // }

            uint16_t num = MAX_PKT_BURST;
            struct rte_mbuf *pkts[num];

            int deq_num = rte_ring_dequeue_burst(global->vhost_tx_rings[vid], (void **)pkts, num, NULL);
            if (deq_num == num) {
                STATS_ADD(ctx->vdev_stats[vid], ring_deq_max_count, 1);
            }
            // LOG_INFO("[%d] Dequeued %d packets from vhost_tx_ring[%d] to vhost_tx_loop\n", ctx->core_id, deq_num,
            // vid);

            if (deq_num > 0) {
                for (int j = 0; j < RTE_MIN(deq_num, 4); j++) {
                    rte_prefetch0(pkts[j]);
                }
                vhost_send(ctx, deq_num, vid, pkts);
            }
        }
    }
}

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts) {
    STATS_ADD(ctx->vdev_stats[vid], call_count, 1);
    int16_t ret = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, pkts, num);
    // CRITICAL: Handle error case (negative return = -1 on error)
    if (ret < 0) {
        ret = 0;
    }

    if (ret < num) {
        // pkts[0 .. ret-1]     -> consumed by vhost (free)
        // pkts[ret .. num-1]   -> STILL OWNED BY YOU -> send back to ring (do not free)
        int enq_num = rte_ring_enqueue_burst(global->vhost_tx_rings[vid], (void **)(pkts + ret), num - ret, NULL);
        if (enq_num < num - ret) {
            // LOG_WARN("[%d](%d) failed to requeue %d packets to vhost_tx_ring[%d]\n", ctx->core_id, vid,
            //          num - ret - enq_num, vid);
            free_pkts(pkts + ret + enq_num, num - ret - enq_num);
        }
        // ctx->vm_bp[vid].state = VM_BLOCKED_TX;
        // ctx->vm_bp[vid].blocked_until_tsc = rte_rdtsc() + BACKOFF_TSC;
        // for (int i = 0; i < num - ret; i++) {
        //     ctx->retry_pkts[vid][i] = pkts[ret + i];
        // }
        // ctx->retry_cnts[vid] = num - ret;
    }
    // else {
    //     // All packets sent successfully - clear backpressure
    //     ctx->vm_bp[vid].state = VM_ACTIVE;
    // }

    STATS_ADD(ctx->vdev_stats[vid], pkt_count, ret);
    if (ret == MAX_PKT_BURST) {
        STATS_ADD(ctx->vdev_stats[vid], max_send_count, 1);
    }

    LOG_VM_OUT("[%d](%d) Sent %d packets to VM\n", ctx->core_id, vid, ret);
    PRINT_PKTS(pkts, ret, LOG_VM_OUT);
    free_pkts(pkts, ret);

    return ret;
}