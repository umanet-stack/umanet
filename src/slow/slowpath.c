#include "src/slow/slowpath.h"
#include "log.h"
#include "src/include/main.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"
#include <generic/rte_cycles.h>
#include <rte_malloc.h>
#include <unistd.h>

void calculate_vhost_rx_plan();
void calculate_vhost_tx_plan();

void slowpath_loop(struct control_ctx *ctx) {
    uint64_t last_dashboard_update = rte_get_tsc_cycles();
    uint64_t last_vhost_plan_update = rte_get_tsc_cycles();
    uint64_t tsc_hz = rte_get_tsc_hz();
    LOG_IMPT("[%u] Entering slowpath loop...\n", ctx->core_id);
    control_tty_init();

    while (1) {
        // STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
        uint64_t cur_tsc = rte_get_tsc_cycles();
        if (config.show_dash && cur_tsc - last_dashboard_update > tsc_hz) {
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
                install_eth_rx_flow(vdev_list_ptr->vdevs[slow_msg->vid]);
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
            rte_mempool_put(control_ctx->msg_pool, slow_msg);
        }

        // calculate new vhost RX plan
        if (cur_tsc - last_vhost_plan_update > tsc_hz) {
            calculate_vhost_rx_plan();
            calculate_vhost_tx_plan();
            last_vhost_plan_update = cur_tsc;
        }
    }
}

// Thresholds for marking a VM as "active" (worth polling constantly)
// These are PER WINDOW (1 second). A VM needs to exceed these thresholds in the sliding window.
// Set high enough to filter out iperf SERVERS (which only send ACKs), but low enough to catch CLIENTS.
// Typical iperf client: ~100K+ packets/sec, Server: <100 packets/sec (just ACKs)
#define BYTE_ACTIVE_THRESHOLD (10 * 1024) // 10 KB per second - more than just ACKs
#define PKT_ACTIVE_THRESHOLD 100          // 100 packets per second - filters out servers
#define PROBE_THRESHOLD 6                 // Probe 6 inactive VMs per core to detect new activity
void calculate_vhost_rx_plan() {
    struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);

    for (int i = 0; i < global->fp_cores; i++) {
        struct vhost_rx_ctx *ctx = fp_ctxs[i]->vhost_rx_ctx;
        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[i]);
        uint8_t is_active[MAX_VHOSTS] = {0};

        for (int j = 0; j < plan->num; j++) {
            uint16_t vid = plan->vids[j];
            if (vdev_list_ptr->vdevs[vid] == NULL) {
                // vdev removed, remove from plan
                continue;
            }

            uint32_t pkt_sum = 0;
            uint32_t byte_sum = 0;
            for (int k = 0; k < WINDOW_SIZE; k++) {
                pkt_sum += ctx->vdev_stats[vid]->pkt_wnd[k];
                byte_sum += ctx->vdev_stats[vid]->byte_wnd[k];
            }

            // count pkt better than count empty polls (bursty = can have many empty polls, few bursts)
            // avoids flapping
            if (byte_sum > BYTE_ACTIVE_THRESHOLD || pkt_sum > PKT_ACTIVE_THRESHOLD) {
                is_active[vid] = 1;
            } else {
                is_active[vid] = 0;
            }

            // update FP core stats in slowpath (don't care atomic/correctness much here)
            ctx->vdev_stats[vid]->wnd_idx = (ctx->vdev_stats[vid]->wnd_idx + 1) % WINDOW_SIZE;
            ctx->vdev_stats[vid]->byte_wnd[ctx->vdev_stats[vid]->wnd_idx] = 0;
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
                LOG_INFO("[%d](%d) vdev active, add to plan\n", i, j);
            }
            // else {
            //     LOG_WARN("[%d](%d) vdev inactive, remove from plan\n", i, j);
            // }
        }
        // add probe vms (check if inactive -> active)
        // Probe ALL inactive VMs that belong to this core to:
        // 1. Detect when they start sending (e.g., iperf client starts)
        // 2. Allow initial connection setup (ARP, DNS, TCP handshake)
        // This is critical for workloads where VMs alternate between active/inactive
        for (int j = 0; j < MAX_VHOSTS && new_plan->num < MAX_PKT_BURST; j++) {
            // inactive, still in vdev_list, and has affinity to this core (was first assigned to this core)
            if (!is_active[j] && vdev_list_ptr->vdevs[j] && vhost_rx_core[j] == i) {
                new_plan->vids[new_plan->num++] = j;
                // Don't log every probe to avoid spam
                // LOG_IMPT("[%d](%d) probe vdev inactive -> active, add to plan\n", i, j);
            }
        }

        struct vhost_plan *old = atomic_exchange_explicit(&vhost_rx_plans[i], new_plan, memory_order_release);
        rte_free(old);
    }
}

void calculate_vhost_tx_plan() {
    struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);

    for (int i = 0; i < global->fp_cores; i++) {
        struct vhost_plan *plan = atomic_load(&vhost_tx_plans[i]);
        uint8_t is_active[MAX_VHOSTS] = {0};

        for (int j = 0; j < plan->num; j++) {
            uint16_t vid = plan->vids[j];
            if (vdev_list_ptr->vdevs[vid] == NULL) {
                // vdev removed, remove from plan
                continue;
            }

            is_active[vid] = 1;
        }

        struct vhost_plan *new_plan = rte_zmalloc("vhost_tx_plan", sizeof(struct vhost_plan), RTE_CACHE_LINE_SIZE);
        if (new_plan == NULL) {
            LOG_ERROR("Failed to allocate memory for new vhost_tx_plan[%d]\n", i);
            return;
        }
        new_plan->num = 0;

        // add active vms
        for (int j = 0; j < MAX_VHOSTS; j++) {
            if (is_active[j]) {
                new_plan->vids[new_plan->num++] = j;
                LOG_INFO("[%d](%d) vdev active, add to plan\n", i, j);
            }
        }

        // Probe ALL VMs assigned to this TX core to detect new activity
        // This is critical - without this, VMs never get added to the plan after initial assignment!
        for (int j = 0; j < MAX_VHOSTS && new_plan->num < MAX_PKT_BURST; j++) {
            // inactive, still in vdev_list, and has affinity to this core
            if (!is_active[j] && vdev_list_ptr->vdevs[j] && vhost_tx_core[j] == i) {
                new_plan->vids[new_plan->num++] = j;
                // Don't log every probe to avoid spam
                // LOG_IMPT("[%d](%d) probe vdev inactive -> active, add to plan\n", i, j);
            }
        }

        struct vhost_plan *old = atomic_exchange_explicit(&vhost_tx_plans[i], new_plan, memory_order_release);
        rte_free(old);
    }
}