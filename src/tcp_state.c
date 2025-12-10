
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Your Name
 */

#include "tcp_state.h"
#include "tcp_offload.h"
#include <rte_malloc.h>

static struct tcp_flow_table *flow_table = NULL;

int tcp_offload_init(uint32_t max_flows) {
    flow_table = rte_zmalloc("tcp_flow_table", sizeof(*flow_table), RTE_CACHE_LINE_SIZE);
    if (!flow_table)
        return -1;

    flow_table->flows = rte_zmalloc("tcp_flows", max_flows * sizeof(struct tcp_flow_state), RTE_CACHE_LINE_SIZE);
    if (!flow_table->flows) {
        rte_free(flow_table);
        return -1;
    }

    flow_table->num_flows = max_flows;

    // For now, just allocate - no actual TCP processing
    return 0;
}

/* Add flow on SYN packet */
struct tcp_flow_state *tcp_flow_create(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t vhost_vid) {
    struct tcp_flow_state *flow;
    uint32_t hash;

    // Simple linear search for now
    for (uint32_t i = 0; i < flow_table->num_flows; i++) {
        flow = &flow_table->flows[i];
        if (flow->state == TCP_STATE_CLOSED) {
            // Found free slot
            flow->local_ip = sip;
            flow->remote_ip = dip;
            flow->local_port = sport;
            flow->remote_port = dport;
            flow->vhost_vid = vhost_vid;
            flow->state = TCP_STATE_SYN_SENT;

            // RTE_LOG(INFO, VHOST_DATA, "Created flow: %08x:%u -> %08x:%u\n", sip, sport, dip, dport);

            return flow;
        }
    }

    return NULL; // Table full
}

struct tcp_flow_state *tcp_flow_lookup(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) {
    struct tcp_flow_state *flow;

    for (uint32_t i = 0; i < flow_table->num_flows; i++) {
        flow = &flow_table->flows[i];
        if (flow->state != TCP_STATE_CLOSED && flow->local_ip == sip && flow->remote_ip == dip &&
            flow->local_port == sport && flow->remote_port == dport) {
            return flow;
        }
    }

    return NULL;
}