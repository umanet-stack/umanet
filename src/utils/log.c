#include "src/utils/utils.h"
#include <stdarg.h>
#include <stdio.h>

static void print_timestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char buffer[9]; // HH:MM:SS\0
    strftime(buffer, sizeof(buffer), "%H:%M:%S", tm_info);
    printf("[%s] ", buffer);
    fflush(stdout);
}

// white
void log_info(const char *fmt, ...) {
    va_list args;        // declare
    va_start(args, fmt); // initialize

    print_timestamp();
    printf(WHITE_PREFIX "[INFO] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
    fflush(stdout);
    va_end(args); // clean up
}

// red
void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    fprintf(stderr, RED_PREFIX "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, RESET_COLOR);
    fflush(stderr);
    va_end(args);
}

// yellow
void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    fprintf(stderr, YELLOW_PREFIX "[WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, RESET_COLOR);
    fflush(stderr);
    va_end(args);
}

// cyan
void log_eth_in(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    printf(CYAN_PREFIX "[ETH IN] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
    fflush(stdout);
    va_end(args);
}

// green
void log_eth_out(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    printf(GREEN_PREFIX "[ETH OUT] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
    fflush(stdout);
    va_end(args);
}

// blue
void log_vm_in(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    printf(BLUE_PREFIX "[VM IN] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
    fflush(stdout);
    va_end(args);
}

// magenta
void log_vm_out(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    print_timestamp();
    printf(MAGENTA_PREFIX "[VM OUT] ");
    vprintf(fmt, args);
    printf(RESET_COLOR);
    fflush(stdout);
    va_end(args);
}