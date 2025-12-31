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

    fprintf(tty_fp, "RX: %d packets/sec\n", rx);
    fprintf(tty_fp, "TX: %d packets/sec\n", tx);
    fprintf(tty_fp, "Drops: %d | VMs: %d\n", drops, vms);

    fflush(tty_fp);
}
