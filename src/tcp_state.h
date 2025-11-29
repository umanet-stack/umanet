#ifndef TCP_STATE_H
#define TCP_STATE_H

#include <rte_mbuf.h>
#include <stdint.h>

/* Simplified TAS flow state for initial integration */
struct tcp_flow_state {
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    /* TX state */
    uint32_t tx_next_seq;
    uint32_t tx_avail;

    /* RX state */
    uint32_t rx_next_seq;
    uint32_t rx_avail;

    /* Connection to vhost device */
    uint16_t vhost_vid;

    /* State flags */
    uint8_t state; // CLOSED, SYN_SENT, ESTABLISHED, etc.
    uint8_t flags;

    int offload_enabled;  // 0 = pass-through, 1 = offloaded
};

enum { TCP_STATE_CLOSED, TCP_STATE_SYN_SENT, TCP_STATE_ESTABLISHED, TCP_STATE_LAST };

/* Flow table - hash table of active TCP connections */
struct tcp_flow_table {
    struct tcp_flow_state *flows;
    uint32_t num_flows;
};

/* Initialize TCP offload subsystem */
int tcp_offload_init(uint32_t max_flows);
void tcp_offload_cleanup(void);
struct tcp_flow_state *tcp_flow_create(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t vhost_vid);

/* Flow lookup - returns NULL if not found */
struct tcp_flow_state *tcp_flow_lookup(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport);

#endif