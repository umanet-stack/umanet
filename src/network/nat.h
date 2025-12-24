#ifndef NAT_H
#define NAT_H

#include <rte_hash.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

// NAT connection tracking entry
struct nat_entry {
    uint32_t vm_ip;     // VM's private IP
    uint16_t vm_port;   // VM's port
    uint32_t ext_ip;    // External (destination) IP
    uint16_t ext_port;  // External (destination) port
    uint16_t nat_port;  // Translated source port (on NIC)
    uint8_t protocol;   // IPPROTO_TCP or IPPROTO_UDP
    uint64_t last_used; // Timestamp for timeout
    int vid;            // Which VM this belongs to
} __rte_cache_aligned;

// NAT connection key (for hash lookup on inbound packets)
struct nat_key {
    uint32_t ext_ip;   // External IP (source of inbound packet)
    uint16_t ext_port; // External port
    uint16_t nat_port; // Our translated port
    uint8_t protocol;  // Protocol
} __rte_packed;

// Initialize NAT subsystem
int nat_init(uint32_t nic_ip);

// Process outbound packet from VM (apply SNAT)
int nat_translate_outbound(struct rte_mbuf *m, int vid, uint32_t nic_ip);

// Process inbound packet from NIC (apply DNAT)
int nat_translate_inbound(struct rte_mbuf *m, int *vid_out);

// Periodic cleanup of expired connections
void nat_cleanup_expired(void);

#endif // NAT_H
