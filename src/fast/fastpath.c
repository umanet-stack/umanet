
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
static inline void cleanup_tx_queue_for_device(struct mbuf_table *tx_q, struct vhost_dev *vdev);

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
    ctx->vhost.tx_q.txq_id = ctx->id;
    ctx->vhost.tx_q.len = 0;

    ctx->stat_cyc_loop = 0;
    ctx->stat_cyc_loop_sleep = 0;

    ctx->stat_cyc_eth_fp = 0;
    ctx->stat_cyc_poll_eth = 0;
    ctx->stat_cyc_send_eth = 0;
    ctx->stat_pkt_eth_rx = 0;
    ctx->stat_pkt_eth_tx = 0;
    ctx->stat_pkt_eth_tx_fail = 0;

    ctx->stat_cyc_vhost_fp = 0;
    ctx->stat_cyc_poll_vhost = 0;
    ctx->stat_cyc_send_vhost = 0;
    ctx->stat_cyc_vdev = 0;
    ctx->stat_pkt_vhost_rx = 0;
    ctx->stat_pkt_vhost_tx = 0;
    ctx->stat_pkt_vhost_tx_fail = 0;

    return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx) {}

void dataplane_loop(struct dataplane_context *ctx) {
    int was_idle = 1;

    unsigned lcore_id = ctx->id;
    struct vhost_dev *vdev;
    struct mbuf_table *tx_q;

    LOG_INFO("Procesing on Core %u started\n", lcore_id);

    tx_q = &ctx->vhost.tx_q;
    tx_q->txq_id = ctx->id;
    LOG_INFO("TX queue ID: %u\n", tx_q->txq_id);

    // Adaptive blocking state (similar to TAS)
    uint64_t last_active_ts = 0;
    int idle_count = 0;
    const uint64_t poll_cycle_tsc = rte_get_tsc_hz() / 1000000; // 1us in TSC cycles

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
        // Track if we received any packets this iteration
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
            continue;
        }

        STATS_TS(eth_fp_start);
        fastpath_from_eth(ctx);
        STATS_TS(eth_fp_end);
        STATS_TSADD(ctx, cyc_eth_fp, eth_fp_end - eth_fp_start);

        STATS_TS(vhost_fp_start);
        for (int i = 0; i < current_device_num; i++) {
            STATS_TS(vdev_start);
            // Use modulo with bounds check to prevent out-of-bounds access
            uint16_t dev_idx = (ctx->vhost.poll_next_device + i) % MAX_VHOST_DEVICES_PER_CORE;

            // Additional safety: ensure dev_idx is within current device count
            if (dev_idx >= current_device_num) {
                dev_idx = i; // Fallback to simple iteration
            }

            vdev = ctx->vhost.vdev_list[dev_idx];

            // Add robust null check
            if (vdev == NULL) {
                LOG_WARN("Warning: NULL vdev at index %d (device_num=%d)\n", dev_idx, current_device_num);
                continue;
            }

            if (unlikely(vdev->remove)) { // device is marked for removal
                LOG_INFO("Removing device vid=%d from dataplane (current device_num=%d)\n", vdev->vid,
                         ctx->vhost.device_num);

                // Clean up any pending TX packets for this device
                cleanup_tx_queue_for_device(&ctx->vhost.tx_q, vdev);

                unlink_vmdq(ctx, vdev);
                vdev->ready = DEVICE_SAFE_REMOVE;

                // Remove from array by shifting remaining elements
                for (int j = dev_idx; j < ctx->vhost.device_num - 1; j++) {
                    ctx->vhost.vdev_list[j] = ctx->vhost.vdev_list[j + 1];
                }
                ctx->vhost.vdev_list[ctx->vhost.device_num - 1] = NULL;
                ctx->vhost.device_num--;

                // Adjust poll_next_device if needed
                if (ctx->vhost.poll_next_device >= ctx->vhost.device_num && ctx->vhost.device_num > 0) {
                    ctx->vhost.poll_next_device = 0;
                }

                LOG_INFO("Device removed, new device_num=%d\n", ctx->vhost.device_num);

                // Update cached value to prevent accessing removed device
                current_device_num = ctx->vhost.device_num;

                // If we removed the last device, break out of loop
                if (current_device_num == 0) {
                    break;
                }

                // Don't increment i since we just shifted elements down
                i--;
                continue;
            }

            // Validate device is in a valid state before polling
            if (vdev->ready != DEVICE_RX && vdev->ready != DEVICE_MAC_LEARNING) {
                LOG_WARN("Warning: Device vid=%d in invalid state %d, skipping\n", vdev->vid, vdev->ready);
                continue;
            }
            STATS_TS(vdev_end);
            STATS_TSADD(ctx, cyc_vdev, vdev_end - vdev_start);

            // TODOZ: Current: Round-robin through all devices, Optimization: Skip idle devices, batch processing
            // Double-check device is still valid before polling TX
            if (likely(!vdev->remove && vdev->ready != DEVICE_SAFE_REMOVE)) {
                // receive packets from VM's TX queue, route them to the NIC or local VM
                fastpath_from_vhost(vdev, ctx);
                // Note: We can't easily get the count here without modifying poll_virtio_tx
                // For now, assume we're busy if we're polling (conservative approach)
                packets_received = 1; // Mark as potentially busy
            }
        }

        // Update round-robin pointer with bounds check
        if (ctx->vhost.device_num > 0) {
            ctx->vhost.poll_next_device = (ctx->vhost.poll_next_device + 1) % ctx->vhost.device_num;
        } else {
            // Reset to 0 when no devices remain
            ctx->vhost.poll_next_device = 0;
        }
        STATS_TS(vhost_fp_end);
        STATS_TSADD(ctx, cyc_vhost_fp, vhost_fp_end - vhost_fp_start);

        was_idle = (packets_received == 0);

        STATS_TS(loop_end);
        STATS_TSADD(ctx, cyc_loop, loop_end - loop_start);
    }
}

// Poll vhost RX queues for incoming packets
static unsigned poll_vhost_rx(struct dataplane_context *ctx, uint32_t ts) {
    int ret;
    unsigned n = 0, total = 0;
    struct rte_mbuf *mbs[BATCH_SIZE];
    struct vhost_dev *vdev;

    n = BATCH_SIZE;
    // Check if the TX buffer has enough free slots, avoid overflow
    if (TXBUF_SIZE - ctx->tx_num < n)
        n = TXBUF_SIZE - ctx->tx_num;

    // Poll multiple vhost devices/queues per core (round-robin)
    for (int i = 0; i < ctx->vhost.device_num && total < n; i++) {
        uint16_t dev_idx = (ctx->vhost.poll_next_device + i) % ctx->vhost.device_num;
        vdev = ctx->vhost.vdev_list[dev_idx];
        if (vdev == NULL)
            continue;

        ret = vhost_poll(ctx, n, vdev->vid, mbs);
        if (ret <= 0)
            continue;
        total += ret;
    }

    // Update round-robin pointer
    if (total > 0 && ctx->vhost.device_num > 0)
        ctx->vhost.poll_next_device = (ctx->vhost.poll_next_device + 1) % ctx->vhost.device_num;

    /* parse packets TCP headers (just timestamp option) to tcpopts */
    // fast_flows_packet_parse(ctx, bhs, fss, tcpopts, n);

    // for (int i = 0; i < n; i++) {
    //     uint16_t vhost_queue = pick_vhost_queue(ctx, mbs[i]);
    //     if (vhost_queue != 0) {
    //         rte_vhost_send(ctx, 1, vdev->vid, &mbs[i]);
    //     }
    // }

    // no. of pkts processed
    return total;
}

// static inline uint16_t pick_vhost_queue(struct dataplane_context *ctx, struct rte_mbuf *pkt) {
//     struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
//     for (int i = 0; i < ctx->vhost.device_num; i++) {
//         if (memcmp(eth->d_addr.addr_bytes, ctx->vhost.vdev_list[i]->mac_address.addr_bytes, 6) == 0)
//             return ctx->rx_queue;
//     }
//     return 0; // optional: drop or broadcast
// }

// Clean up TX queue entries that belong to a device being removed
static inline void cleanup_tx_queue_for_device(struct mbuf_table *tx_q, struct vhost_dev *vdev) {
    if (tx_q == NULL || vdev == NULL) {
        return;
    }

    LOG_INFO("Cleaning up TX queue for device vid=%d (current queue len=%u)\n", vdev->vid, tx_q->len);

    // Note: We can't easily identify which packets belong to which device,
    // so we flush all pending packets to the NIC before device removal
    if (tx_q->len > 0) {
        LOG_INFO("Flushing %u pending packets before device removal\n", tx_q->len);
        struct dataplane_context *ctx = ctxs[vdev->coreid];
        flush_eth_tx(ctx, tx_q);
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
        uint64_t eth_poll = read_stat(&ctx->stat_cyc_poll_eth);
        uint64_t eth_send = read_stat(&ctx->stat_cyc_send_eth);
        uint64_t eth_route = eth_fp - eth_poll - eth_send;
        fprintf(stderr, "ETH FP: \t%" PRIu64 " (%.2f%%)\n", eth_fp, (double)eth_fp / loop * 100);
        fprintf(stderr, "poll_eth: \t%" PRIu64 " (%.2f%%)\n", eth_poll, (double)eth_poll / eth_fp * 100);
        fprintf(stderr, "send_eth: \t%" PRIu64 " (%.2f%%)\n", eth_send, (double)eth_send / eth_fp * 100);
        fprintf(stderr, "route_eth: \t%" PRIu64 " (%.2f%%)\n", eth_route, (double)eth_route / eth_fp * 100);
        fprintf(stderr, "pkt_eth_rx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_rx));
        fprintf(stderr, "pkt_eth_tx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_tx));
        fprintf(stderr, "pkt_eth_tx_fail: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_eth_tx_fail));

        uint64_t vhost_fp = read_stat(&ctx->stat_cyc_vhost_fp);
        uint64_t vhost_poll = read_stat(&ctx->stat_cyc_poll_vhost);
        uint64_t vhost_send = read_stat(&ctx->stat_cyc_send_vhost);
        uint64_t vdev = read_stat(&ctx->stat_cyc_vdev);
        uint64_t vhost_route = vhost_fp - vhost_poll - vhost_send - vdev;
        fprintf(stderr, "VHOST FP: \t%" PRIu64 " (%.2f%%)\n", vhost_fp, (double)vhost_fp / loop * 100);
        fprintf(stderr, "poll_vhost: \t%" PRIu64 " (%.2f%%)\n", vhost_poll, (double)vhost_poll / vhost_fp * 100);
        fprintf(stderr, "send_vhost: \t%" PRIu64 " (%.2f%%)\n", vhost_send, (double)vhost_send / vhost_fp * 100);
        fprintf(stderr, "vdev: \t%" PRIu64 " (%.2f%%)\n", vdev, (double)vdev / vhost_fp * 100);
        fprintf(stderr, "route_vhost: \t%" PRIu64 " (%.2f%%)\n", vhost_route, (double)vhost_route / vhost_fp * 100);
        fprintf(stderr, "pkt_vhost_rx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_rx));
        fprintf(stderr, "pkt_vhost_tx: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx));
        fprintf(stderr, "pkt_vhost_tx_fail: \t%" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx_fail));
    }
}