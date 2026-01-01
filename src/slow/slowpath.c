#include "src/slow/slowpath.h"
#include "src/include/fastpath.h"
#include "src/include/state.h"
#include "src/vhost/vhost.h"
#include <generic/rte_cycles.h>
#include <rte_malloc.h>
#include <unistd.h>

void calculate_vhost_rx_plan();

void slowpath_loop(struct control_ctx *ctx) {
    uint64_t last_dashboard_update = rte_get_tsc_cycles();
    uint64_t last_vhost_rx_plan_update = rte_get_tsc_cycles();
    uint64_t tsc_hz = rte_get_tsc_hz();
    LOG_IMPT("[%u] Entering slowpath loop...\n", ctx->core_id);
    control_tty_init();

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
        uint64_t cur_tsc = rte_get_tsc_cycles();
        if (cur_tsc - last_dashboard_update > tsc_hz) {
            control_dashboard();
            last_dashboard_update = cur_tsc;
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
        if (cur_tsc - last_vhost_rx_plan_update > tsc_hz) {
            calculate_vhost_rx_plan();
            last_vhost_rx_plan_update = cur_tsc;
        }
    }
}

void calculate_vhost_rx_plan() {
    struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);

    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_ctx *ctx = vhost_rx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[i]);
        uint8_t is_active[MAX_VHOSTS] = {0};

        for (int j = 0; j < plan->num; j++) {
            uint16_t vid = plan->vids[j];
            uint32_t pkt_sum = 0;
            for (int k = 0; k < WINDOW_SIZE; k++) {
                pkt_sum += ctx->vdev_stats[vid]->empty_wnd[k];
            }

            // count pkt better than count empty polls (bursty = can have many empty polls, few bursts)
            // avoids flapping
            // may need higher threshold
            if (pkt_sum > 0) {
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

        // add active vms
        for (int j = 0; j < MAX_VHOSTS; j++) {
            if (is_active[j]) {
                new_plan->vids[new_plan->num++] = j;
            }
        }
        // add probe vms (check if inactive -> active)
        for (int j = 0; j < MAX_VHOSTS && new_plan->num < MAX_PKT_BURST; j++) {
            // inactive, still in vdev_list, and has affinity to this core (was first assigned to this core)
            if (!is_active[j] && vdev_list_ptr->vdevs[j] && vhost_rx_core[j] == i) {
                new_plan->vids[new_plan->num++] = j;
                break; // only 1 vm/s
            }
        }

        struct vhost_plan *old = atomic_exchange_explicit(&vhost_rx_plans[i], new_plan, memory_order_release);
        rte_free(old);
    }
}