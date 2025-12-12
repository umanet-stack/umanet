
#include "src/fast/network.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
#include <unistd.h>

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

    return 0;
}

void dataplane_context_destroy(struct dataplane_context *ctx) {}

/*
 * Main function of dataplane. It basically does:
 *
 * for each vhost device {
 *    - drain_eth_rx():
 *      Which drains the host eth Rx queue linked to the vhost device,
 *      and deliver all of them to guest virito Rx ring associated with
 *      this vhost device.
 *
 *    - drain_virtio_tx()
 *      Which drains the guest virtio Tx queue and deliver all of them
 *      to the target, which could be another vhost device, or the
 *      physical eth dev. The route is done in function "virtio_tx_route".
 * }
 */
void dataplane_loop(struct dataplane_context *ctx) {
    unsigned lcore_id = rte_lcore_id();
    struct vhost_dev *vdev;
    struct mbuf_table *tx_q;

    RTE_LOG(INFO, VHOST_DATA, "Procesing on Core %u started\n", lcore_id);

    tx_q = &vhost.lcore_tx_queue[lcore_id];
    // Use ctx->id which matches the initialized TX queue ID
    tx_q->txq_id = ctx->id;

    while (1) {
        sleep(1);
        printf("Draining mbuf table...\n");
        drain_mbuf_table(tx_q); // drain if timeout has elapsed

        /*
         * Inform the configuration core that we have exited the
         * linked list and that no devices are in use if requested.
         */
        if (vhost.lcore_info[lcore_id].dev_removal_flag == REQUEST_DEV_REMOVAL)
            vhost.lcore_info[lcore_id].dev_removal_flag = ACK_DEV_REMOVAL;

        /*
         * Process vhost devices
         */
        TAILQ_FOREACH(vdev, &vhost.lcore_info[lcore_id].vdev_list, lcore_vdev_entry) {
            if (unlikely(vdev->remove)) { // device is marked for removal
                unlink_vmdq(vdev);
                vdev->ready = DEVICE_SAFE_REMOVE;
                continue;
            }

            if (likely(vdev->ready == DEVICE_RX)) {
                printf("Draining eth rx...\n");
                drain_eth_rx(vdev); // receive packets from physical NIC and forward them to a VM
            }

            if (likely(!vdev->remove)) { // device is not being removed (double-check)
                printf("Draining virtio tx...\n");
                drain_virtio_tx(vdev, ctx); // receive packets from VM's TX queue, route them to the correct destination
            }
        }
    }
}
