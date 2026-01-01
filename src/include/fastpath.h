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
#define FLOW_TABLE_SIZE 1024
struct flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    // uint16_t src_port;
    // uint16_t dst_port;
    // uint8_t proto;
};

struct flow_entry {
    struct flow_key key;
    uint16_t eth_tx_core;
    uint64_t last_seen_tsc;
};

#endif /* FASTPATH_H_ */
