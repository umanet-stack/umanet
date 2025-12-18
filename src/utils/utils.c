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
    const char *color_code;
    const char *prefix;

    switch (level) {
    case LOG_PKT_IN:
        color_code = CYAN_PREFIX;
        prefix = "[PKT IN] ";
        break;
    case LOG_PKT_OUT:
        color_code = GREEN_PREFIX;
        prefix = "[PKT OUT] ";
        break;
    case LOG_INFO:
        color_code = WHITE_PREFIX;
        prefix = "[INFO] ";
        break;
    case LOG_ERROR:
        color_code = RED_PREFIX;
        prefix = "[ERROR] ";
        break;
    case LOG_WARN:
        color_code = YELLOW_PREFIX;
        prefix = "[WARN] ";
        break;
    default:
        color_code = WHITE_PREFIX;
        prefix = "";
        break;
    }

    for (int i = 0; i < count; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
        uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

        printf("%s%s", color_code, prefix);
        printf("Packet %d: len=%u, ether_type=0x%04x", i, pkts[i]->pkt_len, ether_type);

        // Identify common packet types
        if (ether_type == RTE_ETHER_TYPE_ARP) {
            printf(" (ARP)");
            // Print ARP details
            if (pkts[i]->pkt_len >= sizeof(struct rte_ether_hdr) + 28) {
                struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
                printf(" op=%u", rte_be_to_cpu_16(arp->arp_opcode));
                if (rte_be_to_cpu_16(arp->arp_opcode) == 1) {
                    printf(" (REQUEST)");
                } else if (rte_be_to_cpu_16(arp->arp_opcode) == 2) {
                    printf(" (REPLY)");
                }
            }
        } else if (ether_type == RTE_ETHER_TYPE_IPV4) {
            printf(" (IPv4)");
            struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(eth + 1);
            printf(" proto=%u", ipv4->next_proto_id);
            if (ipv4->next_proto_id == IPPROTO_ICMP) {
                printf(" (ICMP)");
                // Print ICMP details
                if (pkts[i]->pkt_len >= sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + 4) {
                    uint8_t *icmp = (uint8_t *)(ipv4 + 1);
                    printf(" type=%u code=%u", icmp[0], icmp[1]);
                }
            } else if (ipv4->next_proto_id == IPPROTO_UDP) {
                printf(" (UDP)");
            } else if (ipv4->next_proto_id == IPPROTO_TCP) {
                printf(" (TCP)");
            }
            // Print IP addresses (network byte order)
            uint32_t src_ip = rte_be_to_cpu_32(ipv4->src_addr);
            uint32_t dst_ip = rte_be_to_cpu_32(ipv4->dst_addr);
            printf(" src_ip=%u.%u.%u.%u dst_ip=%u.%u.%u.%u", (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
                   (src_ip >> 8) & 0xff, src_ip & 0xff, (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
                   (dst_ip >> 8) & 0xff, dst_ip & 0xff);
        } else if (ether_type == RTE_ETHER_TYPE_IPV6) {
            printf(" (IPv6)");
        }

        // Print MAC addresses
        printf(" src=");
        for (int j = 0; j < 6; j++) {
            printf("%02x%s", eth->s_addr.addr_bytes[j], j < 5 ? ":" : "");
        }
        printf(" dst=");
        for (int j = 0; j < 6; j++) {
            printf("%02x%s", eth->d_addr.addr_bytes[j], j < 5 ? ":" : "");
        }

        // Reset color and add newline once at the end
        printf(RESET_COLOR "\n");
    }
}