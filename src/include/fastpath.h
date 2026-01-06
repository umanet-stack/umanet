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

static inline void pkts_set_flags(struct rte_mbuf **pkts, unsigned num) {
    // flags = tell driver what to do
    for (unsigned i = 0; i < num; i++) {
        pkts[i]->ol_flags |=
            // IPv4 packet, and the IPv4 header checksum must be computed (TSO requires rewriting IP length per segment)
            RTE_MBUF_F_TX_IPV4 |
            // TCP checksum is not valid yet — compute it after segmentation
            RTE_MBUF_F_TX_TCP_CKSUM | RTE_MBUF_F_TX_IP_CKSUM |
            // TSO enable bit, this mbuf represents multiple TCP segments
            RTE_MBUF_F_TX_TCP_SEG;

        // Split the payload into chunks of this size
        // It does not include TCP/IP headers, only TCP payload.
        // MTU(1500) - IPv4 header(20) - TCP header(20) - TCP timestamp(12) = TCP payload(1448)
        pkts[i]->tso_segsz = 1448; // typically 1448 or derived from MTU

        // It relies on these offsets to find the headers for checksum computation and segmentation
        pkts[i]->l2_len = sizeof(struct rte_ether_hdr);
        pkts[i]->l3_len = sizeof(struct rte_ipv4_hdr);
        pkts[i]->l4_len = sizeof(struct rte_tcp_hdr);
    }
}

#endif /* FASTPATH_H_ */
