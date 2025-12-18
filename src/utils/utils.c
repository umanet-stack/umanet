#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

#include "src/utils/utils.h"

void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}

void print_pkts(struct rte_mbuf **pkts, uint16_t count, enum log_level level) {
    for (int i = 0; i < count; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
        uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

        log_msg(level, "Packet %d: len=%u, ether_type=0x%04x", i, pkts[i]->pkt_len, ether_type);

        // Identify common packet types
        if (ether_type == RTE_ETHER_TYPE_ARP) {
            log_msg(level, " (ARP)");
            // Print ARP details
            if (pkts[i]->pkt_len >= sizeof(struct rte_ether_hdr) + 28) {
                struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
                log_msg(level, " op=%u", rte_be_to_cpu_16(arp->arp_opcode));
                if (rte_be_to_cpu_16(arp->arp_opcode) == 1) {
                    log_msg(level, " (REQUEST)");
                } else if (rte_be_to_cpu_16(arp->arp_opcode) == 2) {
                    log_msg(level, " (REPLY)");
                }
            }
        } else if (ether_type == RTE_ETHER_TYPE_IPV4) {
            log_msg(level, " (IPv4)");
            struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(eth + 1);
            log_msg(level, " proto=%u", ipv4->next_proto_id);
            if (ipv4->next_proto_id == IPPROTO_ICMP) {
                log_msg(level, " (ICMP)");
                // Print ICMP details
                if (pkts[i]->pkt_len >= sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + 4) {
                    uint8_t *icmp = (uint8_t *)(ipv4 + 1);
                    log_msg(level, " type=%u code=%u", icmp[0], icmp[1]);
                }
            } else if (ipv4->next_proto_id == IPPROTO_UDP) {
                log_msg(level, " (UDP)");
            } else if (ipv4->next_proto_id == IPPROTO_TCP) {
                log_msg(level, " (TCP)");
            }
            // Print IP addresses (network byte order)
            uint32_t src_ip = rte_be_to_cpu_32(ipv4->src_addr);
            uint32_t dst_ip = rte_be_to_cpu_32(ipv4->dst_addr);
            log_msg(level, " src_ip=%u.%u.%u.%u dst_ip=%u.%u.%u.%u", (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
                    (src_ip >> 8) & 0xff, src_ip & 0xff, (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
                    (dst_ip >> 8) & 0xff, dst_ip & 0xff);
        } else if (ether_type == RTE_ETHER_TYPE_IPV6) {
            log_msg(level, " (IPv6)");
        }

        log_msg(level, " src=");
        for (int j = 0; j < 6; j++) {
            log_msg(level, "%02x%s", eth->s_addr.addr_bytes[j], j < 5 ? ":" : "");
        }
        log_msg(level, " dst=");
        for (int j = 0; j < 6; j++) {
            log_msg(level, "%02x%s", eth->d_addr.addr_bytes[j], j < 5 ? ":" : "");
        }
        log_msg(level, "\n");
    }
}