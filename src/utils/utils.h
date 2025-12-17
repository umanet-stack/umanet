#ifndef UTILS_H_
#define UTILS_H_

#include <rte_mbuf.h>

void free_pkts(struct rte_mbuf **pkts, uint16_t n);

#endif