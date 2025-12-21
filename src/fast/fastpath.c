
#include "src/include/fastpath.h"
#include "src/fast/internal.h"
#include "src/fast/network.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
#include <rte_mbuf_core.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

static inline void drain_vhost_tx(struct mbuf_table *tx_q);
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

    /* Initialize vhost device array for this context */
    memset(ctx->vhost.vdev_list, 0, sizeof(ctx->vhost.vdev_list));
    ctx->vhost.device_num = 0;
    ctx->vhost.dev_removal_flag = 0;
    ctx->vhost.poll_next_device = 0;

    /* Initialize TX queue */
    memset(&ctx->vhost.tx_q, 0, sizeof(ctx->vhost.tx_q));
    ctx->vhost.tx_q.txq_id = ctx->id;
    ctx->vhost.tx_q.len = 0;

    ctx->stat_cyc_loop = 0;
    ctx->stat_cyc_loop_sleep = 0;
    ctx->stat_cyc_loop_vdev = 0;
    ctx->stat_cyc_loop_vhost = 0;

    ctx->stat_cyc_poll_eth = 0;
    ctx->stat_cyc_flush_eth = 0;
    ctx->stat_cyc_poll_vhost = 0;
    ctx->stat_cyc_route_vhost = 0;
    ctx->stat_cyc_virtio_tx = 0;

    return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx) {}

void dataplane_loop(struct dataplane_context *ctx) {
    struct notify_blockstate nbs;
    uint32_t ts;
    uint64_t cyc, prev_cyc;
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
            drain_vhost_tx(tx_q);
        STATS_TS(drain_vhost_tx_end);
        STATS_TSADD(ctx, cyc_flush_eth, drain_vhost_tx_end - sleep);

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

                unlink_vmdq(vdev);
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
            STATS_TSADD(ctx, cyc_loop_vdev, vdev_end - vdev_start);

            if (likely(vdev->ready == DEVICE_RX)) {
                // receive packets from physical NIC and forward them to a VM
                // Note: poll_eth_rx doesn't return count, but we can check if packets were processed
                // by checking if any packets were forwarded (this is approximate)
                STATS_TS(poll_eth_start);
                poll_eth_rx(vdev);
                STATS_TS(poll_eth_end);
                STATS_TSADD(ctx, cyc_poll_eth, poll_eth_end - poll_eth_start);
            }

            // TODOZ: Current: Round-robin through all devices, Optimization: Skip idle devices, batch processing
            // Double-check device is still valid before polling TX
            if (likely(!vdev->remove && vdev->ready != DEVICE_SAFE_REMOVE)) {
                // receive packets from VM's TX queue, route them to the NIC or local VM
                STATS_TS(loop_vhost_start);
                poll_virtio_tx(vdev, ctx);
                STATS_TS(loop_vhost_end);
                STATS_TSADD(ctx, cyc_loop_vhost, loop_vhost_end - loop_vhost_start);
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

        // Update idle state for adaptive pausing (TAS-style)
        was_idle = (packets_received == 0);

        // /* count cycles of previous iteration if it was busy */
        // prev_cyc = cyc;
        // cyc = rte_get_tsc_cycles();
        // if (!was_idle)
        //     ctx->loadmon_cyc_busy += cyc - prev_cyc;

        // // n += poll_rx(ctx, ts, cyc);
        // STATS_TS(rx);
        // // Flush TX buffer (send pkt)
        // // tx_flush(ctx);

        // n += poll_vhost_rx(ctx, ts);
        // STATS_TS(qs);
        // STATS_TSADD(ctx, cyc_qs, qs - qm);

        // n += poll_rx(ctx, ts, cyc);      // Physical NIC - external traffic (later)
        // n += poll_vhost_rx(ctx, ts);      // Vhost - VM traffic
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

        ret = vhost_poll(&ctx->net, n, vdev->vid, mbs);
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
    //         rte_vhost_enqueue_burst(vhost_queue, VIRTIO_TXQ, &mbs[i], 1);
    //     }
    // }

    // no. of pkts processed
    return total;
}

static inline uint16_t pick_vhost_queue(struct dataplane_context *ctx, struct rte_mbuf *pkt) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    for (int i = 0; i < ctx->vhost.device_num; i++) {
        if (memcmp(eth->d_addr.addr_bytes, ctx->vhost.vdev_list[i]->mac_address.addr_bytes, 6) == 0)
            return ctx->vhost.vdev_list[i]->rx_queue;
    }
    return 0; // optional: drop or broadcast
}

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
        flush_eth_tx(tx_q);
    }
}

// drain into NIC if timeout has elapsed
static inline void drain_vhost_tx(struct mbuf_table *tx_q) {
    // static = function-scope, keeps value between function calls
    static uint64_t prev_tsc; // previous timestamp

    uint64_t cur_tsc = rte_rdtsc();
    if (unlikely(cur_tsc - prev_tsc > MBUF_TABLE_DRAIN_TSC)) {
        prev_tsc = cur_tsc;

        LOG_INFO("TX queue drained after timeout with burst size %u\n", tx_q->len);
        flush_eth_tx(tx_q);
    }
}

static inline uint64_t read_stat(uint64_t *p) { return __sync_lock_test_and_set(p, 0); }

void dataplane_dump_stats(void) {
    struct dataplane_context *ctx;
    unsigned i;

    for (i = 0; i < fp_cores_max; i++) {
        ctx = ctxs[i];
        fprintf(stderr, "\nCORE %u:\n", i);
        fprintf(stderr, "loop: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_loop));
        fprintf(stderr, "\tloop_sleep: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_loop_sleep));
        fprintf(stderr, "\tloop_vdev: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_loop_vdev));
        fprintf(stderr, "\tloop_vhost: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_loop_vhost));
        fprintf(stderr, "poll_eth: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_poll_eth));
        fprintf(stderr, "flush_eth: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_flush_eth));
        fprintf(stderr, "poll_vhost: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_poll_vhost));
        fprintf(stderr, "route_vhost: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_route_vhost));
        fprintf(stderr, "virtio_tx: %" PRIu64 "\n", read_stat(&ctx->stat_cyc_virtio_tx));
        fprintf(stderr, "pkt_vhost_rx: %" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_rx));
        fprintf(stderr, "pkt_vhost_tx: %" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx));
        fprintf(stderr, "pkt_vhost_tx_fail: %" PRIu64 "\n", read_stat(&ctx->stat_pkt_vhost_tx_fail));
    }
}