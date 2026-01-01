#include "src/include/state.h"
#include "utils.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static FILE *tty_fp = NULL;

void print_byte_pkt_sum(struct vhost_plan *plan, struct vdev_rx_stats **stats);
void print_byte_wnd(struct vhost_plan *plan, struct vdev_rx_stats **stats);
void print_pkt_wnd(struct vhost_plan *plan, struct vdev_rx_stats **stats);

void control_tty_init() {
    tty_fp = fopen("/dev/tty", "w");
    if (!tty_fp) {
        perror("fopen(/dev/tty)");
    }
}

void control_dashboard(int rx, int tx, int drops, int vms) {
    // Clear entire terminal
    fprintf(tty_fp, "\033[2J"); // clear screen
    fprintf(tty_fp, "\033[H");  // move cursor to top-left (1;1)

    struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);
    fprintf(tty_fp, "vhosts: %d\n", vdev_list_ptr->num);
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_ctx *ctx = vhost_rx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[i]);
        fprintf(tty_fp, "\nVHOST RX CORE %d (%u): ", i, plan->num);
        uint64_t pkt_count = 0;
        uint64_t call_count = 0;
        uint64_t empty_poll_count = 0;
        uint64_t max_poll_count = 0;
        uint64_t ring_enq_fail_count = 0;

        for (int j = 0; j < MAX_VHOSTS; j++) {
            if (j < plan->num) {
                fprintf(tty_fp, "%d ", plan->vids[j]);
            }
            if (vhost_rx_core[j] == i) {
                pkt_count += ctx->vdev_stats[j]->pkt_count;
                call_count += ctx->vdev_stats[j]->call_count;
                empty_poll_count += ctx->vdev_stats[j]->empty_poll_count;
                max_poll_count += ctx->vdev_stats[j]->max_poll_count;
                ring_enq_fail_count += ctx->vdev_stats[j]->ring_enq_fail_count;
            }
        }
        fprintf(tty_fp, "\n");

        fprintf(tty_fp, "pkt: %s\t", display_number(pkt_count));
        fprintf(tty_fp, "call: %s\t", display_number(call_count));
        fprintf(tty_fp, "empty_poll: %s\t", display_number(empty_poll_count));
        fprintf(tty_fp, "max_poll: %s\t", display_number(max_poll_count));
        fprintf(tty_fp, "ring_enq_fail: %s\n", display_number(ring_enq_fail_count));
        // print_byte_wnd(plan, ctx->vdev_stats);
        // print_pkt_wnd(plan, ctx->vdev_stats);
        print_byte_pkt_sum(plan, ctx->vdev_stats);
    }
    for (int i = 0; i < global->vhost_tx_cores; i++) {
        struct vhost_tx_ctx *ctx = vhost_tx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_tx_plans[i]);
        fprintf(tty_fp, "\nVHOST TX CORE %d (%u): ", i, plan->num);
        uint64_t pkt_count = 0;
        uint64_t call_count = 0;
        uint64_t max_send_count = 0;
        uint64_t send_fail_count = 0;
        uint64_t ring_deq_max_count = 0;

        for (int j = 0; j < MAX_VHOSTS; j++) {
            if (j < plan->num) {
                fprintf(tty_fp, "%d ", plan->vids[j]);
            }
            if (vhost_tx_core[j] == i) {
                pkt_count += ctx->vdev_stats[j]->pkt_count;
                call_count += ctx->vdev_stats[j]->call_count;
                max_send_count += ctx->vdev_stats[j]->max_send_count;
                send_fail_count += ctx->vdev_stats[j]->send_fail_count;
                ring_deq_max_count += ctx->vdev_stats[j]->ring_deq_max_count;
            }
        }
        fprintf(tty_fp, "\n");

        fprintf(tty_fp, "pkt: %s\t", display_number(pkt_count));
        fprintf(tty_fp, "call: %s\t", display_number(call_count));
        fprintf(tty_fp, "max_send: %s\t", display_number(max_send_count));
        fprintf(tty_fp, "send_fail: %s\t", display_number(send_fail_count));
        fprintf(tty_fp, "ring_deq_max: %s\n", display_number(ring_deq_max_count));
    }

    fflush(tty_fp);
}

void print_byte_pkt_sum(struct vhost_plan *plan, struct vdev_rx_stats **stats) {
    for (int i = 0; i < plan->num; i++) {
        uint16_t vid = plan->vids[i];
        uint32_t byte_sum = 0;
        uint32_t pkt_sum = 0;
        for (int j = 0; j < WINDOW_SIZE; j++) {
            byte_sum += stats[vid]->byte_wnd[j];
            pkt_sum += stats[vid]->pkt_wnd[j];
        }
        fprintf(tty_fp, "(%d): %sB, ", vid, display_number(byte_sum));
        fprintf(tty_fp, "%sP ", display_number(pkt_sum));
    }
    fprintf(tty_fp, "\n");
}

void print_byte_wnd(struct vhost_plan *plan, struct vdev_rx_stats **stats) {
    for (int i = 0; i < plan->num; i++) {
        uint16_t vid = plan->vids[i];
        fprintf(tty_fp, "(%d)[", vid);
        for (int j = 0; j < WINDOW_SIZE; j++) {
            fprintf(tty_fp, "%s ", display_number(stats[vid]->byte_wnd[j]));
        }
        fprintf(tty_fp, "] ");
    }
    fprintf(tty_fp, "\n");
}

void print_pkt_wnd(struct vhost_plan *plan, struct vdev_rx_stats **stats) {
    for (int i = 0; i < plan->num; i++) {
        uint16_t vid = plan->vids[i];
        fprintf(tty_fp, "(%d)[", vid);
        for (int j = 0; j < WINDOW_SIZE; j++) {
            fprintf(tty_fp, "%s ", display_number(stats[vid]->pkt_wnd[j]));
        }
        fprintf(tty_fp, "] ");
    }
    fprintf(tty_fp, "\n");
}