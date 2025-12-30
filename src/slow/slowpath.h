#ifndef SLOWPATH_H_
#define SLOWPATH_H_

#include <stdint.h>

enum slow_reason {
    SLOW_ARP,
    SLOW_ND,
    SLOW_UNKNOWN_DST,
};

struct slow_msg {
    enum slow_reason reason;
    uint16_t eth_queue_id; // if from eth
    uint16_t vid;          // if from vhost
    struct rte_mbuf *mbuf;
};

#endif // SLOWPATH_H_