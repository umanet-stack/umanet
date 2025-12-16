
#include "src/fast/fastemu.h"
#include "src/fast/internal.h"
#include "src/fast/network.h"
#include "src/fast/tcp_common.h"
#include "src/include/fastpath.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
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

static void dataplane_block(struct dataplane_context *ctx, uint32_t ts);
static unsigned poll_rx(struct dataplane_context *ctx, uint32_t ts, uint64_t tsc) __attribute__((noinline));
static unsigned poll_vhost_rx(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_kernel(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static unsigned poll_qman_fwd(struct dataplane_context *ctx, uint32_t ts) __attribute__((noinline));
static void poll_scale(struct dataplane_context *ctx);

static inline uint8_t bufcache_prealloc(struct dataplane_context *ctx, uint16_t num,
                                        struct network_buf_handle ***handles);
static inline void bufcache_alloc(struct dataplane_context *ctx, uint16_t num);
static inline void bufcache_free(struct dataplane_context *ctx, struct network_buf_handle *handle);

static inline void tx_flush(struct dataplane_context *ctx);
static inline void tx_send(struct dataplane_context *ctx, struct network_buf_handle *nbh, uint16_t off, uint16_t len);

static void arx_cache_flush(struct dataplane_context *ctx, uint64_t tsc) __attribute__((noinline));

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

    /* initialize queue manager */
    // if (qman_thread_init(ctx) != 0) {
    //     fprintf(stderr, "initializing qman thread failed\n");
    //     return -1;
    // }

    /* initialize network queue */
    if (network_thread_init(ctx) != 0) {
        fprintf(stderr, "initializing rx thread failed\n");
        return -1;
    }

    ctx->poll_next_ctx = ctx->id;

    // ctx->evfd = eventfd(0, EFD_NONBLOCK);
    // assert(ctx->evfd != -1);
    // ctx->ev.epdata.event = EPOLLIN;
    // int r = rte_epoll_ctl(RTE_EPOLL_PER_THREAD, EPOLL_CTL_ADD, ctx->evfd, &ctx->ev);
    // assert(r == 0);
    // fp_state->kctx[ctx->id].evfd = ctx->evfd;

    /* Initialize vhost device array for this context */
    memset(ctx->vhost.vdev_list, 0, sizeof(ctx->vhost.vdev_list));
    ctx->vhost.device_num = 0;
    ctx->vhost.dev_removal_flag = 0;
    ctx->vhost.poll_next_device = 0;

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
        // work counter used to determine if the core was idle.
        // unsigned n = 0;

        // /* count cycles of previous iteration if it was busy */
        // prev_cyc = cyc;
        // cyc = rte_get_tsc_cycles();
        // if (!was_idle)
        //     ctx->loadmon_cyc_busy += cyc - prev_cyc;

        // ts = qman_timestamp(cyc);
        // STATS_TS(start);

        // // n += poll_rx(ctx, ts, cyc);
        // STATS_TS(rx);
        // // Flush TX buffer (send pkt)
        // // tx_flush(ctx);

        // n += poll_vhost_rx(ctx, ts);
        // STATS_TS(qs);
        // STATS_TSADD(ctx, cyc_qs, qs - qm);

        // n += poll_rx(ctx, ts, cyc);      // Physical NIC - external traffic (later)
        // n += poll_vhost_rx(ctx, ts);      // Vhost - VM traffic
        sleep(1);
        printf("Draining mbuf table...\n");
        // tx_flush
        drain_mbuf_table(tx_q); // drain if timeout has elapsed

        /*
         * Inform the configuration core that we have exited the
         * linked list and that no devices are in use if requested.
         */
        if (ctx->vhost.dev_removal_flag == REQUEST_DEV_REMOVAL)
            ctx->vhost.dev_removal_flag = ACK_DEV_REMOVAL;

        /*
         * Process vhost devices
         */
        for (int i = 0; i < ctx->vhost.device_num; i++) {
            vdev = ctx->vhost.vdev_list[i];
            if (vdev == NULL)
                continue;

            if (unlikely(vdev->remove)) { // device is marked for removal
                unlink_vmdq(vdev);
                vdev->ready = DEVICE_SAFE_REMOVE;
                // Remove from array by shifting remaining elements
                for (int j = i; j < ctx->vhost.device_num - 1; j++) {
                    ctx->vhost.vdev_list[j] = ctx->vhost.vdev_list[j + 1];
                }
                ctx->vhost.vdev_list[ctx->vhost.device_num - 1] = NULL;
                ctx->vhost.device_num--;
                i--; // Adjust index after removal
                continue;
            }

            if (likely(vdev->ready == DEVICE_RX)) {
                printf("Draining eth rx...\n");
                // poll_rx
                drain_eth_rx(vdev); // receive packets from physical NIC and forward them to a VM
            }

            if (likely(!vdev->remove)) { // device is not being removed (double-check)
                printf("Draining virtio tx...\n");
                // poll_queues
                drain_virtio_tx(vdev, ctx); // receive packets from VM's TX queue, route them to the correct destination
            }
        }
    }
}

// Poll vhost RX queues for incoming packets
static unsigned poll_vhost_rx(struct dataplane_context *ctx, uint32_t ts) {
    int ret;
    unsigned n = 0, i, j, total = 0;
    void *fss[BATCH_SIZE];
    struct tcp_opts tcpopts[BATCH_SIZE];
    struct network_buf_handle *bhs[BATCH_SIZE];
    struct vhost_dev *vdev;

    n = BATCH_SIZE;
    // Check if the TX buffer has enough free slots, avoid overflow
    if (TXBUF_SIZE - ctx->tx_num < n)
        n = TXBUF_SIZE - ctx->tx_num;

    // Poll multiple vhost devices/queues per core (round-robin)
    for (j = 0; j < ctx->vhost.device_num && total < n; j++) {
        uint16_t dev_idx = (ctx->vhost.poll_next_device + j) % ctx->vhost.device_num;
        vdev = ctx->vhost.vdev_list[dev_idx];
        if (vdev == NULL)
            continue;

        ret = vhost_poll(&ctx->net, n, vdev->vid, bhs);
        if (ret <= 0)
            continue;
        total += n;

        // Look up flow states
        fast_flows_packet_fss(ctx, bhs + (total - n), fss + (total - n), n);

        // Parse TCP headers
        fast_flows_packet_parse(ctx, bhs + (total - n), fss + (total - n), tcpopts + (total - n), n);

        // Process packets
        for (i = total - n; i < total; i++) {
            if (fss[i] != NULL) {
                ret = fast_flows_packet(ctx, bhs[i], fss[i], &tcpopts[i], ts);
                // Instead of writing to shared RX buffer, queue for vhost TX
                if (ret > 0) {
                    // Determine which vhost device this packet came from
                    // vhost_tx_enqueue(vring, ctx->id, bhs[i]);
                }
            } else {
                // New connection - send to slowpath
                fast_kernel_packet(ctx, bhs[i]);
            }
        }
    }

    // Update round-robin pointer
    if (total > 0 && ctx->vhost.device_num > 0)
        ctx->vhost.poll_next_device = (ctx->vhost.poll_next_device + 1) % ctx->vhost.device_num;

    return total;
}