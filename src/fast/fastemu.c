
#include "src/fast/network.h"
#include "src/include/tas.h"

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
