#include "src/include/fastpath.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf_core.h>

#define RING_ALMOST_FULL RING_SIZE * 0.8
#define ECN_START (RING_SIZE * 1 / 2)   // 50%
#define ECN_MID (RING_SIZE * 7 / 10)    // 70%
#define ECN_HIGH (RING_SIZE * 85 / 100) // 85%

static inline void ecn_mark_packet(struct rte_mbuf *pkt);

static inline void ecn_mark_packets(struct rte_mbuf **mbufs, int num, int congestion,
                                    uint32_t *rr) // per-core counter for probabilistic randomness
{
    if (congestion < ECN_START)
        return;

    int step;
    if (congestion < ECN_MID) {
        step = 16; // 50–70% congestion, ~6% ecn marks
    } else if (congestion < ECN_HIGH) {
        step = 4; // 70–85% congestion, ~25% ecn marks
    } else {
        step = 1; // 85%+ congestion, 100% ecn marks
    }

    for (int i = 0; i < num; i++) {
        if (((*rr)++ % step) == 0)
            ecn_mark_packet(mbufs[i]);
    }
}

static inline void ecn_mark_packet(struct rte_mbuf *pkt) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return;

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

    if ((ip->type_of_service & 0x03) != 0) {
        ip->type_of_service |= 0x03; // CE
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
    }
}