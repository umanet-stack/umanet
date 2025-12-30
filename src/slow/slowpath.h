#ifndef SLOWPATH_H_
#define SLOWPATH_H_

#include <rte_arp.h>
#include <rte_mbuf_core.h>
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

static inline int is_arp_req(struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    return arp->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
}

#endif // SLOWPATH_H_