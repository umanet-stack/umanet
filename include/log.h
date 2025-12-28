#ifndef LOG_H_
#define LOG_H_

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

void print_pkts(struct rte_mbuf **pkts, uint16_t count, enum log_level level);

#define LOG_IMPT(fmt, ...) log_info(fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_error(fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_warn(fmt, ##__VA_ARGS__)
#define PRINT_PKTS_WARN(pkts, count, level) print_pkts(pkts, count, level)

#ifdef DEBUG
#define LOG_ETH_IN(fmt, ...) log_eth_in(fmt, ##__VA_ARGS__)
#define LOG_ETH_OUT(fmt, ...) log_eth_out(fmt, ##__VA_ARGS__)
#define LOG_VM_IN(fmt, ...) log_vm_in(fmt, ##__VA_ARGS__)
#define LOG_VM_OUT(fmt, ...) log_vm_out(fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_info(fmt, ##__VA_ARGS__)
#define PRINT_PKTS(pkts, count, level) print_pkts(pkts, count, level)
#else
#define LOG_ETH_IN(fmt, ...)                                                                                           \
    do {                                                                                                               \
    } while (0)
#define LOG_ETH_OUT(fmt, ...)                                                                                          \
    do {                                                                                                               \
    } while (0)
#define LOG_VM_IN(fmt, ...)                                                                                            \
    do {                                                                                                               \
    } while (0)
#define LOG_VM_OUT(fmt, ...)                                                                                           \
    do {                                                                                                               \
    } while (0)
#define LOG_INFO(fmt, ...)                                                                                             \
    do {                                                                                                               \
    } while (0)
#define PRINT_PKTS(pkts, count, level)                                                                                 \
    do {                                                                                                               \
    } while (0)
#endif

#endif /* LOG_H_ */