#ifndef UTILS_H_
#define UTILS_H_

#include <rte_mbuf.h>

enum log_level { LOG_PKT_IN, LOG_PKT_OUT, LOG_INFO, LOG_ERROR, LOG_WARN };

void log_msg(enum log_level level, const char *fmt, ...);
void log_pkt_in(const char *fmt, ...);
void log_pkt_out(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_warn(const char *fmt, ...);

void free_pkts(struct rte_mbuf **pkts, uint16_t n);
void print_pkts(struct rte_mbuf **pkts, uint16_t count, enum log_level level);

#endif