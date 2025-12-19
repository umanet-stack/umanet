#include "src/utils/utils.h"
#include <stdarg.h>
#include <stdio.h>

// white
void log_info(const char *fmt, ...) {
    va_list args;        // declare
    va_start(args, fmt); // initialize

    printf(WHITE_PREFIX "[INFO] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);

    va_end(args); // clean up
}

// red
void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, RED_PREFIX "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, RESET_COLOR);

    va_end(args);
}

// yellow
void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, YELLOW_PREFIX "[WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, RESET_COLOR);

    va_end(args);
}

// cyan
void log_eth_in(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf(CYAN_PREFIX "[ETH IN] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);

    va_end(args);
}

// green
void log_eth_out(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf(GREEN_PREFIX "[ETH OUT] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);

    va_end(args);
}

// blue
void log_vm_in(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf(BLUE_PREFIX "[VM IN] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);

    va_end(args);
}

// magenta
void log_vm_out(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf(MAGENTA_PREFIX "[VM OUT] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
}