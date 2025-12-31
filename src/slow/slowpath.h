#ifndef SLOWPATH_H_
#define SLOWPATH_H_

#include "src/include/state.h"
#include <rte_arp.h>
#include <rte_mbuf_core.h>
#include <stdint.h>

enum slow_reason {
    SLOW_ARP_REQ,
    SLOW_MAC_LEARNING,
    SLOW_UNKNOWN_DST,
};

enum slow_src {
    SLOW_SRC_ETH,
    SLOW_SRC_VHOST,
};

struct slow_msg {
    enum slow_reason reason;
    enum slow_src src;
    uint16_t eth_queue_id; // if from eth
    uint16_t vid;          // if from vhost
    struct rte_mbuf *mbuf;
};

int process_arp_req(struct control_ctx *ctx, uint16_t vid, struct rte_mbuf *m, enum slow_src src);
void slowpath_loop(struct control_ctx *ctx);

void control_tty_init(void);
void control_dashboard(int rx, int tx, int drops, int vms);

#endif // SLOWPATH_H_