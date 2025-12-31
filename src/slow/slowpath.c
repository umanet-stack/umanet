#include "src/slow/slowpath.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/vhost/vhost.h"
#include <unistd.h>

void slowpath_loop(struct control_ctx *ctx) {
    int last_update_time = 0;
    LOG_IMPT("[%u] Entering slowpath loop...\n", ctx->core_id);
    control_tty_init();

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        if (rte_get_tsc_cycles() - last_update_time > 100000000000) { // 1 second
            control_dashboard();
            last_update_time = rte_get_tsc_cycles();
        }

        struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);
        uint16_t num = MAX_PKT_BURST;
        struct slow_msg *slow_msgs[num];
        int enq_num = rte_ring_dequeue_burst(global->slowpath_ring, (void **)slow_msgs, num, NULL);
        for (int i = 0; i < enq_num; i++) {
            struct slow_msg *slow_msg = slow_msgs[i];

            switch (slow_msg->reason) {
            case SLOW_MAC_LEARNING:
                if (vdev_list_ptr->vdevs[slow_msg->vid] == NULL) {
                    LOG_ERROR("vdev_list->vdevs[%d] is NULL\n", slow_msg->vid);
                    continue;
                }
                link_vmdq(vdev_list_ptr->vdevs[slow_msg->vid], slow_msg->mbuf);
                break;

            case SLOW_ARP_REQ:
                if (vdev_list_ptr->vdevs[slow_msg->vid] == NULL) {
                    LOG_ERROR("vdev_list->vdevs[%d] is NULL for ARP request\n", slow_msg->vid);
                    continue;
                }
                process_arp_req(ctx, slow_msg->vid, slow_msg->mbuf, slow_msg->src);
                break;

            default:
                break;
            }
        }
    }
}
