#include "src/utils/utils.h"
#include <stdarg.h>
#include <stdio.h>

// white
void log_info(const char *fmt, ...) {
    va_list args;        // declare
    va_start(args, fmt); // initialize

    printf("\033[37m[INFO] ");
    vprintf(fmt, args);
    printf("\033[0m\n");

    va_end(args); // clean up
}

// red
void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf("\033[31m[ERROR] ");
    vprintf(fmt, args);
    printf("\033[0m\n");

    va_end(args);
}

// yellow
void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf("\033[33m[WARN] ");
    vprintf(fmt, args);
    printf("\033[0m\n");

    va_end(args);
}

// cyan
void log_pkt_in(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf("\033[36m[PKT IN] ");
    vprintf(fmt, args);
    printf("\033[0m\n");

    va_end(args);
}

// green
void log_pkt_out(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printf("\033[32m[PKT OUT] ");
    vprintf(fmt, args);
    printf("\033[0m\n");

    va_end(args);
}

void log_msg(enum log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    switch (level) {
    case LOG_INFO:
        log_info(fmt, args);
        break;
    case LOG_ERROR:
        log_error(fmt, args);
        break;
    case LOG_WARN:
        log_warn(fmt, args);
        break;
    case LOG_PKT_IN:
        log_pkt_in(fmt, args);
        break;
    case LOG_PKT_OUT:
        log_pkt_out(fmt, args);
        break;
    default:
        log_error("Invalid log level: %d", level);
        break;
    }

    va_end(args);
}