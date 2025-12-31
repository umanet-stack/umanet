#include "utils.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>

int util_parse_ipv4(const char *s, uint32_t *ip) {
    if (inet_pton(AF_INET, s, ip) != 1) {
        return -1;
    }
    *ip = htonl(*ip);
    return 0;
}

// display as K, M, B, T
char *display_number(uint64_t number) {
    static char buf[16]; // static so pointer remains valid
    if (number < 1000) {
        snprintf(buf, sizeof(buf), "%lu", number);
    } else if (number < 1000000) {
        snprintf(buf, sizeof(buf), "%.1fK", number / 1000.0);
    } else if (number < 1000000000) {
        snprintf(buf, sizeof(buf), "%.1fM", number / 1000000.0);
    } else if (number < 1000000000000ULL) {
        snprintf(buf, sizeof(buf), "%.1fB", number / 1000000000.0);
    } else {
        snprintf(buf, sizeof(buf), "%.2fT", number / 1000000000000.0);
    }
    return buf;
}