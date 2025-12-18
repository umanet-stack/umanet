
#include "src/include/fastpath.h"
#include "src/fast/internal.h"
#include "src/fast/network.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
#include <rte_mbuf_core.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

#define DATAPLANE_TSCS

#ifdef DATAPLANE_STATS
#ifdef DATAPLANE_TSCS
#define STATS_TS(n) uint64_t n = rte_get_tsc_cycles()
#define STATS_TSADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#endif
#define STATS_ADD(c, f, n) __sync_fetch_and_add(&c->stat_##f, n)
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#define STATS_ADD(c, f, n)                                                                                             \
    do {                                                                                                               \
    } while (0)
#endif

static inline void drain_vhost_tx(struct mbuf_table *tx_q);
static inline void cleanup_tx_queue_for_device(struct mbuf_table *tx_q, struct vhost_dev *vdev);

int dataplane_init(void) {
    if (FLEXNIC_INTERNAL_MEM_SIZE < sizeof(struct flextcp_pl_mem)) {
        fprintf(stderr,
                "dataplane_init: internal flexnic memory size not "
                "sufficient (got %x, need %zx)\n",
                FLEXNIC_INTERNAL_MEM_SIZE, sizeof(struct flextcp_pl_mem));
        return -1;
    }

    if (fp_cores_max > FLEXNIC_PL_APPST_CTX_MCS) {
        fprintf(stderr,
                "dataplane_init: more cores than FLEXNIC_PL_APPST_CTX_MCS "
                "(%u)\n",
                FLEXNIC_PL_APPST_CTX_MCS);
        return -1;
    }
    if (FLEXNIC_PL_FLOWST_NUM > FLEXNIC_NUM_QMQUEUES) {
        fprintf(stderr,
                "dataplane_init: more flow states than queue manager queues"
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
        fprintf(stderr, "initializing rte_ring_create");
        return -1;
    }

    /* initialize network queue */
    if (network_thread_init(ctx) != 0) {
        fprintf(stderr, "initializing rx thread failed\n");
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

    printf("Procesing on Core %u started\n", lcore_id);

    tx_q = &ctx->vhost.tx_q;
    tx_q->txq_id = ctx->id;
    printf("TX queue ID: %u\n", tx_q->txq_id);

    while (!exited) {
        // Use usleep for more responsive device removal handling
        // 100ms sleep instead of 1 second
        // usleep(100000); // 100ms
        sleep(1);

        printf("Draining TX queue into NIC...\n");
        if (tx_q->len > 0)
            drain_vhost_tx(tx_q);

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
            continue;
        }

        for (int i = 0; i < current_device_num; i++) {
            // Use modulo with bounds check to prevent out-of-bounds access
            uint16_t dev_idx = (ctx->vhost.poll_next_device + i) % MAX_VHOST_DEVICES_PER_CORE;

            // Additional safety: ensure dev_idx is within current device count
            if (dev_idx >= current_device_num) {
                dev_idx = i; // Fallback to simple iteration
            }

            vdev = ctx->vhost.vdev_list[dev_idx];

            // Add robust null check
            if (vdev == NULL) {
                printf("Warning: NULL vdev at index %d (device_num=%d)\n", dev_idx, current_device_num);
                continue;
            }

            if (unlikely(vdev->remove)) { // device is marked for removal
                printf("Removing device vid=%d from dataplane (current device_num=%d)\n", vdev->vid,
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

                printf("Device removed, new device_num=%d\n", ctx->vhost.device_num);

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
                printf("Warning: Device vid=%d in invalid state %d, skipping\n", vdev->vid, vdev->ready);
                continue;
            }

            if (likely(vdev->ready == DEVICE_RX)) {
                printf("Polling eth rx...\n");
                // receive packets from physical NIC and forward them to a VM
                poll_eth_rx(vdev);
            }

            // Double-check device is still valid before polling TX
            if (likely(!vdev->remove && vdev->ready != DEVICE_SAFE_REMOVE)) {
                printf("Polling virtio tx...\n");
                // receive packets from VM's TX queue, route them to the NIC or local VM
                poll_virtio_tx(vdev, ctx);
            }
        }

        // Update round-robin pointer with bounds check
        if (ctx->vhost.device_num > 0) {
            ctx->vhost.poll_next_device = (ctx->vhost.poll_next_device + 1) % ctx->vhost.device_num;
        } else {
            // Reset to 0 when no devices remain
            ctx->vhost.poll_next_device = 0;
        }

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

    printf("Cleaning up TX queue for device vid=%d (current queue len=%u)\n", vdev->vid, tx_q->len);

    // Note: We can't easily identify which packets belong to which device,
    // so we flush all pending packets to the NIC before device removal
    if (tx_q->len > 0) {
        printf("Flushing %u pending packets before device removal\n", tx_q->len);
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

        printf("TX queue drained after timeout with burst size %u\n", tx_q->len);
        flush_eth_tx(tx_q);
    }
}
