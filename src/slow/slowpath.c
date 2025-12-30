#include "src/slow/slowpath.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <unistd.h>

void slowpath_loop(struct control_ctx *ctx) {
    LOG_IMPT("[%u] Entering slowpath loop...\n", ctx->core_id);

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        uint16_t num = MAX_PKT_BURST;
        struct slow_msg *slow_msgs[num];
        int enq_num = rte_ring_dequeue_burst(global->slowpath_ring, (void **)slow_msgs, num, NULL);
        for (int i = 0; i < enq_num; i++) {
            struct slow_msg *slow_msg = slow_msgs[i];
            switch (slow_msg->reason) {
            case SLOW_MAC_LEARNING:
                break;
            case SLOW_ARP_REQ:
                process_arp_req(ctx, slow_msg->vid, slow_msg->mbuf, slow_msg->src);
                break;
            default:
                break;
            }
        }
    }
}
