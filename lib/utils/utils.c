#include "utils.h"
#include <arpa/inet.h>
#include <stdint.h>

int util_parse_ipv4(const char *s, uint32_t *ip) {
    if (inet_pton(AF_INET, s, ip) != 1) {
        return -1;
    }
    *ip = htonl(*ip);
    return 0;
}