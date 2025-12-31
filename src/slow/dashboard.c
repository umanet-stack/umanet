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
    fprintf(tty_fp, "\033[s"); // save cursor position

    fprintf(tty_fp, "\033[1;1H\033[2KRX: %d packets/sec", rx);
    fprintf(tty_fp, "\033[2;1H\033[2KTX: %d packets/sec", tx);
    fprintf(tty_fp, "\033[3;1H\033[2KDrops: %d | VMs: %d", drops, vms);

    fprintf(tty_fp, "\033[u"); // restore cursor
    fflush(tty_fp);
}
