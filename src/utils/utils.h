#ifndef UTILS_H_
#define UTILS_H_

#include <rte_mbuf.h>

#define RED_PREFIX "\033[31m"
#define YELLOW_PREFIX "\033[33m"
#define GREEN_PREFIX "\033[32m"
#define BLUE_PREFIX "\033[34m"
#define MAGENTA_PREFIX "\033[35m"
#define CYAN_PREFIX "\033[36m"
#define WHITE_PREFIX "\033[37m"
#define RESET_COLOR "\033[0m"

enum log_level { LOG_INFO, LOG_ERROR, LOG_WARN, LOG_ETH_IN, LOG_ETH_OUT, LOG_VM_IN, LOG_VM_OUT };

void log_eth_in(const char *fmt, ...);
void log_eth_out(const char *fmt, ...);
void log_vm_in(const char *fmt, ...);
void log_vm_out(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_warn(const char *fmt, ...);

void free_pkts(struct rte_mbuf **pkts, uint16_t n);
void print_pkts(struct rte_mbuf **pkts, uint16_t count, enum log_level level);

int util_parse_ipv4(const char *s, uint32_t *ip);

#endif