#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include <rte_ring.h>
#include <stdatomic.h>
#include <unistd.h>

static inline unsigned vhost_send(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);
static inline unsigned vhost_resend(struct vhost_tx_ctx *ctx, unsigned num, unsigned vid, struct rte_mbuf **pkts);

void vhost_tx_loop(struct vhost_tx_ctx *ctx) {
    LOG_IMPT("[%u] Entering vhost_tx loop...\n", ctx->core_id);

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

            if (ctx->vm_bp[vid].state == VM_BLOCKED_TX && rte_rdtsc() < ctx->vm_bp[vid].blocked_until_tsc)
                continue;

            if (ctx->retry_cnts[vid] > 0) {
                vhost_resend(ctx, ctx->retry_cnts[vid], vid, ctx->retry_pkts[vid]);
                continue;
            }

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