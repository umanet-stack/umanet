
#include "src/include/fastpath.h"
#include "log.h"
#include "src/fast/internal.h"
#include "src/include/tas.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"
#include <rte_mbuf_core.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

static inline void drain_vhost_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q);

int dataplane_init(void) {
    if (FLEXNIC_INTERNAL_MEM_SIZE < sizeof(struct flextcp_pl_mem)) {
        LOG_ERROR("dataplane_init: internal flexnic memory size not "
                  "sufficient (got %x, need %zx)\n",
                  FLEXNIC_INTERNAL_MEM_SIZE, sizeof(struct flextcp_pl_mem));
        return -1;
    }

    if (fp_cores_max > FLEXNIC_PL_APPST_CTX_MCS) {
        LOG_ERROR("dataplane_init: more cores than FLEXNIC_PL_APPST_CTX_MCS "
                  "(%u)\n",
                  FLEXNIC_PL_APPST_CTX_MCS);
        return -1;
    }
    if (FLEXNIC_PL_FLOWST_NUM > FLEXNIC_NUM_QMQUEUES) {
        LOG_ERROR("dataplane_init: more flow states than queue manager queues"
                  "(%u > %u)\n",
                  FLEXNIC_PL_FLOWST_NUM, FLEXNIC_NUM_QMQUEUES);
        return -1;
    }

    return 0;
}

int dataplane_context_init(struct dataplane_context *ctx) {
    char name[32];

    /* initialize forwarding queue */
    sprintf(name, "qman_fwd_ring_%u", ctx->id);
    if ((ctx->qman_fwd_ring = rte_ring_create(name, 32 * 1024, rte_socket_id(), RING_F_SC_DEQ)) == NULL) {
        LOG_ERROR("initializing rte_ring_create failed\n");
        return -1;
    }

    /* initialize network queue */
    if (network_thread_init(ctx) != 0) {
        LOG_ERROR("initializing rx thread failed\n");
        return -1;
    }

    ctx->poll_next_ctx = ctx->id;

    memset(ctx->vhost.vdev_list, 0, sizeof(ctx->vhost.vdev_list));
    ctx->vhost.device_num = 0;
    ctx->vhost.dev_removal_flag = 0;
    ctx->vhost.poll_next_device = 0;

    memset(&ctx->vhost.tx_q, 0, sizeof(ctx->vhost.tx_q));
    ctx->vhost.tx_q.len = 0;

    ctx->stat_cyc_loop = 0;
    ctx->stat_cyc_loop_sleep = 0;

    ctx->stat_cyc_eth_fp = 0;
    ctx->stat_cyc_eth_poll = 0;
    ctx->stat_cyc_eth_sort = 0;
    ctx->stat_cyc_eth_tx_vm = 0;

    ctx->stat_cyc_vhost_fp = 0;
    ctx->stat_cyc_vhost_vdev = 0;
    ctx->stat_cyc_vhost_vmdq = 0;
    ctx->stat_cyc_vhost_poll = 0;
    ctx->stat_cyc_vhost_route = 0;
    ctx->stat_cyc_vhost_route_init = 0;
    ctx->stat_cyc_vhost_sort = 0;
    ctx->stat_cyc_vhost_tx_eth = 0;
    ctx->stat_cyc_vhost_tx_vm = 0;
    ctx->stat_cyc_vhost_broadcast = 0;

    ctx->stat_pkt_eth_rx = 0;
    ctx->stat_pkt_eth_tx = 0;
    ctx->stat_pkt_eth_tx_fail = 0;
    ctx->stat_pkt_vhost_rx = 0;
    ctx->stat_pkt_vhost_tx = 0;
    ctx->stat_pkt_vhost_tx_fail = 0;

    return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx) {}

void dataplane_loop(struct dataplane_context *ctx) {
    struct mbuf_table *tx_q = &ctx->vhost.tx_q;
    LOG_INFO("Procesing on Core %u started\n", ctx->id);

    // Adaptive blocking state
    int was_idle = 1;
    uint64_t last_active_ts = 0;
    int idle_count = 0;
    const uint64_t poll_cycle_tsc = rte_get_tsc_hz() / 1000000; // 1us in TSC cycles
    uint64_t cyc;                                               // TSC cycles for adaptive pause

    while (!exited) {
        STATS_TS(loop_start);
#ifdef DEBUG
        sleep(1);
#else
        // Adaptive pause: only pause when idle to allow vhost-user state sync
        // Similar to TAS's adaptive blocking, but using rte_pause() instead of epoll
        // since vhost-user doesn't support eventfd notifications
        cyc = rte_get_tsc_cycles();
        if (was_idle) {
            idle_count++;
            // After being idle for multiple iterations, pause to allow vhost-user sync
            // This gives the vhost-user backend time to update shared memory
            if (idle_count > 2 || (cyc - last_active_ts > poll_cycle_tsc)) {
                rte_pause();
            }
        } else {
            idle_count = 0;
            last_active_ts = cyc;
        }
#endif
        STATS_TS(sleep);
        STATS_TSADD(ctx, cyc_loop_sleep, sleep - loop_start);
        unsigned packets_received = 0;

        // Drain TX queue if it has packets (check is cheap, only drain on timeout)
        if (tx_q->len > 0)
            drain_vhost_tx(ctx, tx_q);

        /*
         * Inform the configuration core that we have exited the
         * linked list and that no devices are in use if requested.
         */
        if (ctx->vhost.dev_removal_flag == REQUEST_DEV_REMOVAL)
            ctx->vhost.dev_removal_flag = ACK_DEV_REMOVAL;

        // Cache device_num to avoid race conditions during removal
        int current_device_num = ctx->vhost.device_num;

        // If no devices, skip polling
        if (current_device_num == 0) {
            was_idle = 1;
            STATS_TS(loop_end);
            STATS_TSADD(ctx, cyc_loop, loop_end - loop_start);
            continue;
        }

        STATS_TS(eth_fp_start);
        fastpath_from_eth(ctx);
        STATS_TS(eth_fp_end);
        STATS_TSADD(ctx, cyc_eth_fp, eth_fp_end - eth_fp_start);

        STATS_TS(vhost_fp_start);
        packets_received = fastpath_from_vhost(ctx, current_device_num);
        STATS_TS(vhost_fp_end);
        STATS_TSADD(ctx, cyc_vhost_fp, vhost_fp_end - vhost_fp_start);

        was_idle = (packets_received == 0);
        STATS_TS(loop_end);
        STATS_TSADD(ctx, cyc_loop, loop_end - loop_start);
    }
}

// drain into NIC if timeout has elapsed
static inline void drain_vhost_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q) {
    // static = function-scope, keeps value between function calls
    static uint64_t prev_tsc; // previous timestamp

    uint64_t cur_tsc = rte_rdtsc();
    if (unlikely(cur_tsc - prev_tsc > MBUF_TABLE_DRAIN_TSC)) {
        prev_tsc = cur_tsc;

        LOG_INFO("TX queue drained after timeout with burst size %u\n", tx_q->len);
        flush_eth_tx(ctx, tx_q);
    }
}

static inline uint64_t read_stat(uint64_t *p) { return __sync_lock_test_and_set(p, 0); }

void dataplane_dump_stats(void) {
    struct dataplane_context *ctx;
    unsigned i;

    for (i = 0; i < fp_cores_max; i++) {
        ctx = ctxs[i];
        uint64_t loop = read_stat(&ctx->stat_cyc_loop);
        uint64_t loop_sleep = read_stat(&ctx->stat_cyc_loop_sleep);
        fprintf(stderr, "\n========== CORE %u ==========\n", i);
        fprintf(stderr, "FASTPATH:\n");
        fprintf(stderr, "whole loop: \t%" PRIu64 "\n", loop);
        fprintf(stderr, "loop_sleep: \t%" PRIu64 " (%.2f%%)\n", loop_sleep, (double)loop_sleep / loop * 100);

        uint64_t eth_fp = read_stat(&ctx->stat_cyc_eth_fp);
        uint64_t eth_poll = read_stat(&ctx->stat_cyc_eth_poll);
        uint64_t eth_sort = read_stat(&ctx->stat_cyc_eth_sort);
        uint64_t eth_tx_vm = read_stat(&ctx->stat_cyc_eth_tx_vm);
        fprintf(stderr, "\nETH FP: \t%" PRIu64 " (%.2f%% of whole loop)\n", eth_fp, (double)eth_fp / loop * 100);
        fprintf(stderr, "eth_poll: \t%" PRIu64 " (%.2f%%)\n", eth_poll, (double)eth_poll / eth_fp * 100);
        fprintf(stderr, "eth_sort: \t%" PRIu64 " (%.2f%%)\n", eth_sort, (double)eth_sort / eth_fp * 100);
        fprintf(stderr, "eth_tx_vm: \t%" PRIu64 " (%.2f%%)\n", eth_tx_vm, (double)eth_tx_vm / eth_fp * 100);
        fprintf(stderr, "TOTAL ETH: \t %.2f%%\n", (double)(eth_poll + eth_sort + eth_tx_vm) / eth_fp * 100);

        uint64_t vhost_fp = read_stat(&ctx->stat_cyc_vhost_fp);
        uint64_t vhost_vdev = read_stat(&ctx->stat_cyc_vhost_vdev);
        uint64_t vhost_vmdq = read_stat(&ctx->stat_cyc_vhost_vmdq);
        uint64_t vhost_poll = read_stat(&ctx->stat_cyc_vhost_poll);
        uint64_t vhost_route = read_stat(&ctx->stat_cyc_vhost_route);
        uint64_t vhost_route_init = read_stat(&ctx->stat_cyc_vhost_route_init);
        uint64_t vhost_sort = read_stat(&ctx->stat_cyc_vhost_sort);
        uint64_t vhost_tx_eth = read_stat(&ctx->stat_cyc_vhost_tx_eth);
        uint64_t vhost_tx_vm = read_stat(&ctx->stat_cyc_vhost_tx_vm);
        uint64_t vhost_broadcast = read_stat(&ctx->stat_cyc_vhost_broadcast);
        fprintf(stderr, "\nVHOST FP: \t%" PRIu64 " (%.2f%% of whole loop)\n", vhost_fp, (double)vhost_fp / loop * 100);
        fprintf(stderr, "vhost_vdev: \t%" PRIu64 " (%.2f%%)\n", vhost_vdev, (double)vhost_vdev / vhost_fp * 100);
        fprintf(stderr, "vhost_vmdq: \t%" PRIu64 " (%.2f%%)\n", vhost_vmdq, (double)vhost_vmdq / vhost_fp * 100);
        fprintf(stderr, "vhost_poll: \t%" PRIu64 " (%.2f%%)\n", vhost_poll, (double)vhost_poll / vhost_fp * 100);
        fprintf(stderr, "vhost_route: \t%" PRIu64 " (%.2f%%)\n", vhost_route, (double)vhost_route / vhost_fp * 100);
        fprintf(stderr, "\t vhost_route_init: \t%" PRIu64 " (%.2f%%)\n", vhost_route_init,
                (double)vhost_route_init / vhost_route * 100);
        fprintf(stderr, "\t vhost_sort: \t%" PRIu64 " (%.2f%%)\n", vhost_sort, (double)vhost_sort / vhost_route * 100);
        fprintf(stderr, "\t vhost_tx_eth: \t%" PRIu64 " (%.2f%%)\n", vhost_tx_eth,
                (double)vhost_tx_eth / vhost_route * 100);
        fprintf(stderr, "\t vhost_tx_vm: \t%" PRIu64 " (%.2f%%)\n", vhost_tx_vm,
                (double)vhost_tx_vm / vhost_route * 100);
        fprintf(stderr, "\t vhost_broad: \t%" PRIu64 " (%.2f%%)\n", vhost_broadcast,
                (double)vhost_broadcast / vhost_route * 100);
        // Calculate route overhead (time not accounted for by sub-operations)
        uint64_t vhost_route_subops = vhost_route_init + vhost_sort + vhost_broadcast + vhost_tx_eth + vhost_tx_vm;
        uint64_t vhost_route_overhead = (vhost_route > vhost_route_subops) ? (vhost_route - vhost_route_subops) : 0;
        fprintf(stderr, "vhost_route_overhead: \t%" PRIu64 " (%.2f%%)\n", vhost_route_overhead,
                (double)vhost_route_overhead / vhost_fp * 100);

        fprintf(stderr, "TOTAL VHOST: \t %.2f%%\n",
                (double)(vhost_vdev + vhost_vmdq + vhost_poll + vhost_route) / vhost_fp * 100);

        fprintf(stderr, "\npkt_eth_rx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_rx));
        fprintf(stderr, "pkt_eth_tx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_tx));
        fprintf(stderr, "pkt_eth_tx_fail: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_tx_fail));
        fprintf(stderr, "pkt_vhost_rx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_rx));
        fprintf(stderr, "pkt_vhost_tx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx));
        fprintf(stderr, "pkt_vhost_tx_fail: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx_fail));
    }
}