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

#include "src/include/state.h"
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

enum vm_state {
    VM_ACTIVE,
    VM_BLOCKED_TX,
    VM_IDLE_RX,
};

// backpressure for vms
struct vm_bp {
    enum vm_state state;
    uint64_t blocked_until_tsc;
    uint32_t empty_polls;
};

#define BACKOFF_TSC 100000 // 100 us
extern struct vm_bp vm_bp[MAX_VHOSTS];
void vm_bp_init(void);

#endif /* FASTPATH_H_ */
