
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

static inline void drain_vhost_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q, uint16_t queue_len_before);

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
    ctx->prev_tsc = 0;

    memset(ctx->vhost.vdev_list, 0, sizeof(ctx->vhost.vdev_list));
    ctx->vhost.device_num = 0;
    ctx->vhost.dev_removal_flag = 0;
    ctx->vhost.poll_next_device = 0;

    // Initialize active device tracking
    memset(ctx->vhost.active_devices, 0, sizeof(ctx->vhost.active_devices));
    ctx->vhost.active_count = 0;
    ctx->vhost.inactive_check_counter = 0;

    memset(&ctx->vhost.tx_q, 0, sizeof(ctx->vhost.tx_q));
    ctx->vhost.tx_q.len = 0;

    ctx->stat_cyc_loop = 0;

    ctx->stat_cyc_eth_fp = 0;
    ctx->stat_cyc_eth_poll = 0;

    ctx->stat_cyc_vhost_fp = 0;
    ctx->stat_cou_vhost_poll_max = 0;
    ctx->stat_cyc_vhost_poll = 0;
    ctx->stat_cou_vhost_external = 0;
    ctx->stat_cou_vhost_arp = 0;
    ctx->stat_cou_vhost_local = 0;
    ctx->stat_cou_vhost_broadcast = 0;

    ctx->stat_pkt_eth_rx = 0;
    ctx->stat_call_eth_rx = 0;
    ctx->stat_pkt_eth_tx = 0;
    ctx->stat_call_eth_tx = 0;
    ctx->stat_pkt_eth_tx_fail = 0;

    ctx->stat_pkt_vhost_rx = 0;
    ctx->stat_call_vhost_rx = 0;
    ctx->stat_pkt_vhost_tx = 0;
    ctx->stat_call_vhost_tx = 0;
    ctx->stat_pkt_vhost_tx_fail = 0;

    ctx->stat_tx_drain_calls = 0;
    ctx->stat_tx_drain_timeout = 0;
    ctx->stat_tx_drain_full = 0;
    ctx->stat_tx_q_max_depth = 0;
    ctx->stat_eth_tx_partial = 0;
    ctx->stat_loop_iterations = 0;

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

    while (!exited) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
// #else
//         // Adaptive pause: only pause when idle to allow vhost-user state sync
//         // Similar to TAS's adaptive blocking, but using rte_pause() instead of epoll
//         // since vhost-user doesn't support eventfd notifications
//         if (was_idle) {
//             idle_count++;
//             // After being idle for multiple iterations, pause to allow vhost-user sync
//             // This gives the vhost-user backend time to update shared memory
//             if (idle_count > 2 || (start - last_active_ts > poll_cycle_tsc)) {
//                 rte_pause();
//             }
//         } else {
//             idle_count = 0;
//             last_active_ts = start;
//         }
#endif
        unsigned packets_received = 0;
        ctx->stat_loop_iterations++;

        // Track max queue depth (lightweight, no TSC)
        if (tx_q->len > ctx->stat_tx_q_max_depth) {
            ctx->stat_tx_q_max_depth = tx_q->len;
        }

        // Drain TX queue if it has packets (check is cheap, only drain on timeout)
        if (tx_q->len > 0) {
            uint16_t queue_len_before = tx_q->len;
            drain_vhost_tx(ctx, tx_q, queue_len_before);
        }

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
            rte_pause();
            continue;
        }

        fastpath_from_eth(ctx);
        STATS_TS(eth_fp);
        STATS_TSADD(ctx, cyc_eth_fp, eth_fp - start);

        packets_received = fastpath_from_vhost(ctx, current_device_num);
        STATS_TS(vhost_fp);
        STATS_TSADD(ctx, cyc_vhost_fp, vhost_fp - eth_fp);

        was_idle = (packets_received == 0);
        STATS_TSADD(ctx, cyc_loop, vhost_fp - start);
    }
}

// drain into NIC if timeout has elapsed
static inline void drain_vhost_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q, uint16_t queue_len_before) {
    ctx->stat_tx_drain_calls++; // Lightweight counter

    uint64_t cur_tsc = rte_rdtsc();
    if (unlikely(cur_tsc - ctx->prev_tsc > MBUF_TABLE_DRAIN_TSC)) {
        ctx->prev_tsc = cur_tsc;
        ctx->stat_tx_drain_timeout++; // Drained due to timeout

        if (queue_len_before >= MAX_PKT_BURST) {
            ctx->stat_tx_drain_full++; // Queue was full when drained
        }

        // LOG_INFO("TX queue drained after timeout with burst size %u\n", tx_q->len);
        flush_eth_tx(ctx, tx_q);
    } else if (queue_len_before >= MAX_PKT_BURST) {
        // Queue full but timeout hasn't elapsed - drain anyway
        ctx->stat_tx_drain_full++;
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
        fprintf(stderr, "\n========== CORE %u ==========\n", i);
        fprintf(stderr, "FASTPATH:\n");
        fprintf(stderr, "whole loop: \t%" PRIu64 "\n", loop);

        uint64_t eth_fp = read_stat(&ctx->stat_cyc_eth_fp);
        // uint64_t eth_poll = read_stat(&ctx->stat_cyc_eth_poll);
        fprintf(stderr, "\nETH FP: \t%" PRIu64 " (%.2f%% of whole loop)\n", eth_fp, (double)eth_fp / loop * 100);
        // fprintf(stderr, "eth_poll: \t%" PRIu64 " (%.2f%%)\n", eth_poll, (double)eth_poll / eth_fp * 100);
        // fprintf(stderr, "TOTAL ETH: \t %.2f%%\n", (double)(eth_poll) / eth_fp * 100);

        uint64_t vhost_fp = read_stat(&ctx->stat_cyc_vhost_fp);
        // uint64_t vhost_poll = read_stat(&ctx->stat_cyc_vhost_poll);
        fprintf(stderr, "\nVHOST FP: \t%" PRIu64 " (%.2f%% of whole loop)\n", vhost_fp, (double)vhost_fp / loop * 100);
        // fprintf(stderr, "vhost_poll: \t%" PRIu64 " (%.2f%%)\n", vhost_poll, (double)vhost_poll / vhost_fp * 100);
        // fprintf(stderr, "TOTAL VHOST: \t %.2f%%\n", (double)(vhost_poll) / vhost_fp * 100);

        uint64_t vhost_external = read_stat(&ctx->stat_cou_vhost_external);
        uint64_t vhost_arp = read_stat(&ctx->stat_cou_vhost_arp);
        uint64_t vhost_local = read_stat(&ctx->stat_cou_vhost_local);
        uint64_t vhost_broadcast = read_stat(&ctx->stat_cou_vhost_broadcast);
        fprintf(stderr, "vhost_external: \t%" PRIu64 "\n", vhost_external);
        fprintf(stderr, "vhost_arp: \t%" PRIu64 "\n", vhost_arp);
        fprintf(stderr, "vhost_local: \t%" PRIu64 "\n", vhost_local);
        fprintf(stderr, "vhost_broadcast: \t%" PRIu64 "\n", vhost_broadcast);

        uint64_t pkt_eth_rx = read_stat(&ctx->stat_pkt_eth_rx);
        uint64_t pkt_eth_tx = read_stat(&ctx->stat_pkt_eth_tx);
        uint64_t call_eth_rx = read_stat(&ctx->stat_call_eth_rx);
        uint64_t call_eth_tx = read_stat(&ctx->stat_call_eth_tx);
        uint64_t pkt_eth_tx_fail = read_stat(&ctx->stat_pkt_eth_tx_fail);
        uint64_t pkt_vhost_rx = read_stat(&ctx->stat_pkt_vhost_rx);
        uint64_t pkt_vhost_tx = read_stat(&ctx->stat_pkt_vhost_tx);
        uint64_t call_vhost_rx = read_stat(&ctx->stat_call_vhost_rx);
        uint64_t call_vhost_tx = read_stat(&ctx->stat_call_vhost_tx);
        uint64_t pkt_vhost_tx_fail = read_stat(&ctx->stat_pkt_vhost_tx_fail);
        uint64_t cou_vhost_poll_max = read_stat(&ctx->stat_cou_vhost_poll_max);
        fprintf(stderr, "\npkt_eth_rx: \t%" PRIu64 " (avg %.2f pkt/call).\n", pkt_eth_rx,
                (double)pkt_eth_rx / call_eth_rx);
        fprintf(stderr, "pkt_eth_tx: \t%" PRIu64 " (avg %.2f pkt/call).\n", pkt_eth_tx,
                (double)pkt_eth_tx / call_eth_tx);
        fprintf(stderr, "pkt_eth_tx_fail: \t%" PRIu64 "\n", pkt_eth_tx_fail);
        fprintf(stderr, "pkt_vhost_rx: \t%" PRIu64 " (avg %.2f pkt/call).\n", pkt_vhost_rx,
                (double)pkt_vhost_rx / call_vhost_rx);
        fprintf(stderr, "cou_vhost_poll_max: \t%" PRIu64 " (%.2f%% of call_vhost_rx)\n", cou_vhost_poll_max,
                (double)cou_vhost_poll_max / call_vhost_rx * 100);
        fprintf(stderr, "pkt_vhost_tx: \t%" PRIu64 " (avg %.2f pkt/call).\n", pkt_vhost_tx,
                (double)pkt_vhost_tx / call_vhost_tx);
        fprintf(stderr, "pkt_vhost_tx_fail: \t%" PRIu64 "\n", pkt_vhost_tx_fail);

        fprintf(stderr, "\nBOTTLENECK DETECTION (lightweight counters):\n");
        uint64_t loop_iters = read_stat(&ctx->stat_loop_iterations);
        uint64_t tx_drain_calls = read_stat(&ctx->stat_tx_drain_calls);
        uint64_t tx_drain_timeout = read_stat(&ctx->stat_tx_drain_timeout);
        uint64_t tx_drain_full = read_stat(&ctx->stat_tx_drain_full);
        uint64_t tx_q_max = read_stat(&ctx->stat_tx_q_max_depth);
        uint64_t eth_tx_partial = read_stat(&ctx->stat_eth_tx_partial);

        fprintf(stderr, "loop_iterations: \t%" PRIu64 "\n", loop_iters);
        fprintf(stderr, "tx_drain_calls: \t%" PRIu64 " (%.2f%% of loops)\n", tx_drain_calls,
                loop_iters > 0 ? (double)tx_drain_calls / loop_iters * 100 : 0);
        fprintf(stderr, "tx_drain_timeout: \t%" PRIu64 "\n", tx_drain_timeout);
        fprintf(stderr, "tx_drain_full: \t%" PRIu64 " (%.2f%% of drain calls)\n", tx_drain_full,
                tx_drain_calls > 0 ? (double)tx_drain_full / tx_drain_calls * 100 : 0);
        fprintf(stderr, "tx_q_max_depth: \t%" PRIu64 " (max=%u)\n", tx_q_max, MAX_PKT_BURST);
        fprintf(stderr, "eth_tx_partial: \t%" PRIu64 " (TX queue partial sends)\n", eth_tx_partial);
    }
}