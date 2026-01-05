#include "src/include/fastpath.h"
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf_core.h>

#define RING_ALMOST_FULL RING_SIZE * 0.8

static inline void ecn_mark_packet(struct rte_mbuf *pkt);

static inline void ecn_mark_packets(struct rte_mbuf **mbufs, int num, int congestion) {
    if (congestion > RING_ALMOST_FULL) {
        // ring 80%+ full, mark all packets
        for (int i = 0; i < num; i++) {
            ecn_mark_packet(mbufs[i]);
        }
    } else if (congestion > RING_SIZE * 3 / 5) {
        // ring 60%+ full, mark every 4th packet
        for (int i = 0; i < num; i++) {
            if ((i & 3) == 0)
                ecn_mark_packet(mbufs[i]);
        }
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