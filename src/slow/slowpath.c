#include "src/slow/slowpath.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/vhost/vhost.h"
#include <generic/rte_cycles.h>
#include <rte_malloc.h>
#include <unistd.h>

#define IDLE_THRESHOLD 2
void calculate_vhost_rx_plan();

void slowpath_loop(struct control_ctx *ctx) {
    uint64_t last_update_time = rte_get_tsc_cycles();
    uint64_t tsc_hz = rte_get_tsc_hz();
    LOG_IMPT("[%u] Entering slowpath loop...\n", ctx->core_id);
    control_tty_init();

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
        uint64_t cur_tsc = rte_get_tsc_cycles();
        if (cur_tsc - last_update_time > tsc_hz) {
            control_dashboard();
            last_update_time = cur_tsc;
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

        // calculate new vhost RX plan
        // if (cur_tsc - last_update_time > tsc_hz) {
        //     calculate_vhost_rx_plan();
        //     last_update_time = cur_tsc;
        // }
    }
}

void calculate_vhost_rx_plan() {
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_ctx *ctx = vhost_rx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[i]);
        uint8_t is_active[MAX_VHOSTS] = {0};

        for (int j = 0; j < plan->num; j++) {
            uint16_t vid = plan->vids[j];
            uint32_t empty_sum = 0;
            for (int k = 0; k < WINDOW_SIZE; k++) {
                empty_sum += ctx->vdev_stats[vid]->empty_wnd[k];
            }

            if (empty_sum > IDLE_THRESHOLD) {
                is_active[vid] = 0;
            } else {
                is_active[vid] = 1;
            }

            ctx->vdev_stats[vid]->wnd_idx = (ctx->vdev_stats[vid]->wnd_idx + 1) % WINDOW_SIZE;
            ctx->vdev_stats[vid]->pkt_wnd[ctx->vdev_stats[vid]->wnd_idx] = 0;
            ctx->vdev_stats[vid]->empty_wnd[ctx->vdev_stats[vid]->wnd_idx] = 0;
        }

        struct vhost_plan *new_plan = rte_zmalloc("vhost_rx_plan", sizeof(struct vhost_plan), RTE_CACHE_LINE_SIZE);
        if (new_plan == NULL) {
            LOG_ERROR("Failed to allocate memory for new vhost_rx_plan[%d]\n", i);
            return;
        }

        new_plan->num = 0;
        for (int j = 0; j < MAX_VHOSTS; j++) {
            if (is_active[j]) {
                new_plan->vids[new_plan->num++] = j;
            }
        }
        atomic_store_explicit(&vhost_rx_plans[i], new_plan, memory_order_release);
    }
}