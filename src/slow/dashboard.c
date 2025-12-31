#include "src/include/state.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static FILE *tty_fp = NULL;

void control_tty_init(void) {
    tty_fp = fopen("/dev/tty", "w");
    if (!tty_fp) {
        perror("fopen(/dev/tty)");
    }
}

void control_log(const char *fmt, ...) {
    if (!tty_fp)
        return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(tty_fp, fmt, ap);
    va_end(ap);

    fflush(tty_fp);
}

void control_log_status(uint16_t num_vms, uint64_t rx, uint64_t tx) {
    control_log("\r\033[32mSTATUS\033[0m VMs=%u RX=%lu TX=%lu", num_vms, rx, tx);
}

void control_dashboard(int rx, int tx, int drops, int vms) {
    // Clear entire terminal
    fprintf(tty_fp, "\033[2J"); // clear screen
    fprintf(tty_fp, "\033[H");  // move cursor to top-left (1;1)

    struct vdev_list *vdev_list_ptr = atomic_load(&vdev_list);
    fprintf(tty_fp, "vhosts: %d\n", vdev_list_ptr->num);
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        fprintf(tty_fp, "\nVHOST RX CORE %d: ", i);
        struct vhost_rx_ctx *ctx = vhost_rx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_rx_plans[i]);
        uint64_t pkt_count = 0;
        uint64_t call_count = 0;
        uint64_t empty_poll_count = 0;
        uint64_t max_poll_count = 0;
        uint64_t ring_enq_fail_count = 0;

        for (int j = 0; j < plan->num; j++) {
            fprintf(tty_fp, "%d ", plan->vids[j]);
            pkt_count += ctx->vdev_stats[plan->vids[j]]->pkt_count;
            call_count += ctx->vdev_stats[plan->vids[j]]->call_count;
            empty_poll_count += ctx->vdev_stats[plan->vids[j]]->empty_poll_count;
            max_poll_count += ctx->vdev_stats[plan->vids[j]]->max_poll_count;
            ring_enq_fail_count += ctx->vdev_stats[plan->vids[j]]->ring_enq_fail_count;
        }
        fprintf(tty_fp, "\n");

        fprintf(tty_fp, "pkt: %lu\t", pkt_count);
        fprintf(tty_fp, "call: %lu\t", call_count);
        fprintf(tty_fp, "empty_poll: %lu\t", empty_poll_count);
        fprintf(tty_fp, "max_poll: %lu\t", max_poll_count);
        fprintf(tty_fp, "ring_enq_fail: %lu\n", ring_enq_fail_count);
    }
    for (int i = 0; i < global->vhost_tx_cores; i++) {
        fprintf(tty_fp, "\nVHOST TX CORE %d: ", i);
        struct vhost_tx_ctx *ctx = vhost_tx_ctxs[i];
        struct vhost_plan *plan = atomic_load(&vhost_tx_plans[i]);
        uint64_t pkt_count = 0;
        uint64_t call_count = 0;
        uint64_t max_send_count = 0;
        uint64_t send_fail_count = 0;
        uint64_t ring_deq_max_count = 0;

        for (int j = 0; j < plan->num; j++) {
            fprintf(tty_fp, "%d ", plan->vids[j]);
            pkt_count += ctx->vdev_stats[plan->vids[j]]->pkt_count;
            call_count += ctx->vdev_stats[plan->vids[j]]->call_count;
            max_send_count += ctx->vdev_stats[plan->vids[j]]->max_send_count;
            send_fail_count += ctx->vdev_stats[plan->vids[j]]->send_fail_count;
            ring_deq_max_count += ctx->vdev_stats[plan->vids[j]]->ring_deq_max_count;
        }
        fprintf(tty_fp, "\n");

        fprintf(tty_fp, "pkt: %lu\t", pkt_count);
        fprintf(tty_fp, "call: %lu\t", call_count);
        fprintf(tty_fp, "max_send: %lu\t", max_send_count);
        fprintf(tty_fp, "send_fail: %lu\t", send_fail_count);
        fprintf(tty_fp, "ring_deq_max: %lu\n", ring_deq_max_count);
    }

    fflush(tty_fp);
}
