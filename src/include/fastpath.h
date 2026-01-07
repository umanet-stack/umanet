/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef FASTPATH_H_
#define FASTPATH_H_

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <stdbool.h>
#include <stdint.h>

#include <rte_interrupts.h>

#define DATAPLANE_TSCS

#ifdef DATAPLANE_STATS
#ifdef DATAPLANE_TSCS
// USE SPARINGLY, it is partially serializing, forces the CPU to drain speculation
// The CPU cannot overlap work before and after rdtsc.
// In a large function, this kills instruction-level parallelism.
#define STATS_TS(n) uint64_t n = rte_get_tsc_cycles()
// Use regular addition instead of atomic (stats are per-core, no contention)
#define STATS_TSADD(c, f, n) (c->f += (n))
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#endif
#define STATS_ADD(c, f, n) __sync_fetch_and_add(&c->f, n)
#else
#define STATS_TS(n)                                                                                                    \
    do {                                                                                                               \
    } while (0)
#define STATS_TSADD(c, f, n)                                                                                           \
    do {                                                                                                               \
    } while (0)
#define STATS_ADD(c, f, n)                                                                                             \
    do {                                                                                                               \
    } while (0)
#endif

#define MAX_PKT_BURST 32
#define RING_SIZE 4096

#define GRO_MAX_FLOWS 2048
#define GRO_MAX_ITEMS_PER_FLOW 32

// tells NIC to segment TCP packets into smaller segments
static inline void pkts_set_tso_flags(struct rte_mbuf **pkts, unsigned num) {
    // flags = tell driver what to do
    for (unsigned i = 0; i < num; i++) {
        struct rte_mbuf *m = pkts[i];

        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

        if (eth_type == RTE_ETHER_TYPE_IPV4) {
            struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

            if (ip->next_proto_id == IPPROTO_TCP) {
                // struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));
                // Only TCP over IPv4 gets TSO
                m->ol_flags |=
                    // IPv4 packet, and the IPv4 header checksum must be computed (TSO requires rewriting IP length per
                    // segment)
                    RTE_MBUF_F_TX_IPV4 |
                    // TCP checksum is not valid yet — compute it after segmentation
                    RTE_MBUF_F_TX_TCP_CKSUM | RTE_MBUF_F_TX_IP_CKSUM |
                    // TSO enable bit, this mbuf represents multiple TCP segments
                    RTE_MBUF_F_TX_TCP_SEG;

                // Split the payload into chunks of this size
                // It does not include TCP/IP headers, only TCP payload.
                // MTU(1500) - IPv4 header(20) - TCP header(20) - TCP timestamp(12) = TCP payload(1448)
                m->tso_segsz = 1448; // TCP payload per segment

                // It relies on these offsets to find the headers for checksum computation and segmentation
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
                m->l4_len = sizeof(struct rte_tcp_hdr);
                m->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP;
                m->outer_l2_len = 0;
                m->outer_l3_len = 0;
            } else if (ip->next_proto_id == IPPROTO_UDP) {
                // UDP over IPv4 — only compute checksums, no TSO
                m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
            } else {
                // Other IPv4 protocols — just IPv4 checksum
                m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
            }
        } else {
            // Non-IPv4 (ARP, etc.) — no flags
            m->ol_flags = 0;
        }
    }
}

// rte_gro_reassemble_burst relies on these flags to merge packets back together
static inline void pkts_set_gro_flags(struct rte_mbuf **pkts, unsigned num) {
    for (unsigned i = 0; i < num; i++) {
        struct rte_mbuf *m = pkts[i];

        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

        if (eth_type == RTE_ETHER_TYPE_IPV4) {
            struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

            if (ip->next_proto_id == IPPROTO_TCP) {
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
                m->l4_len = sizeof(struct rte_tcp_hdr);
                m->packet_type = RTE_PTYPE_L2_ETHER | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_TCP;
                m->outer_l2_len = 0;
                m->outer_l3_len = 0;
            } else if (ip->next_proto_id == IPPROTO_UDP) {
                // UDP over IPv4 — only compute checksums, no TSO
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
            } else {
                // Other IPv4 protocols — just IPv4 checksum
                m->l2_len = sizeof(struct rte_ether_hdr);
                m->l3_len = sizeof(struct rte_ipv4_hdr);
            }
        }
    }
}

static inline void clear_tx_offloads(struct rte_mbuf *m) {
    m->ol_flags &= ~(RTE_MBUF_F_TX_TCP_SEG | RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM |
                     RTE_MBUF_F_TX_UDP_CKSUM);
}
static inline void fix_ipv4_cksum(struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
}
static inline void fix_tcp_cksum(struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    // Use ip->ihl (header length in 4-byte words) to handle IP options
    struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + (ip->ihl * 4));

    tcp->cksum = 0;
    tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
}
static inline void fix_udp_cksum(struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    // Use ip->ihl (header length in 4-byte words) to handle IP options
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + (ip->ihl * 4));

    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
}

#endif /* FASTPATH_H_ */
