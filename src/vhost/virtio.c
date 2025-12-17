/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "src/include/fastpath.h"
#include "src/utils/utils.h"
#include "src/vhost/vhost.h"

#include <generic/rte_cycles.h>
#include <netinet/in.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>

const uint16_t vlan_tags[64] = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
    1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
    1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
    1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
};

// receive packets from VM's TX queue, route them to the correct destination
void poll_virtio_tx(struct vhost_dev *vdev, struct dataplane_context *ctx) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    // copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
    count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, ctx->net.pool, pkts, MAX_PKT_BURST);

    // Debug: print device state
    if (count > 0) {
        printf("[Device vid=%d state=%d] Received %d packets from VM's TX queue\n", vdev->vid, vdev->ready, count);
    }

    for (int i = 0; i < count; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
        uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

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

        printf(" src=");
        for (int j = 0; j < 6; j++) {
            printf("%02x%s", eth->s_addr.addr_bytes[j], j < 5 ? ":" : "");
        }
        printf(" dst=");
        for (int j = 0; j < 6; j++) {
            printf("%02x%s", eth->d_addr.addr_bytes[j], j < 5 ? ":" : "");
        }
        printf("\n");
    }

    /* setup VMDq for the first packet */
    if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) { // device in MAC learning
        printf("[Device vid=%d] In MAC learning mode, processing first packet\n", vdev->vid);
        if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1) { // failed to learn MAC from first packet
            printf("[Device vid=%d] MAC learning failed, dropping %d packets\n", vdev->vid, count);
            free_pkts(pkts, count);
            return; // Early return after freeing packets
        }
        printf("[Device vid=%d] MAC learning successful, device now in RX mode\n", vdev->vid);
    }

    for (i = 0; i < count; ++i) {                                               // loop received packets
        virtio_tx_route(vdev, pkts[i], &ctx->vhost.tx_q, vlan_tags[vdev->vid]); // route each to correct destination
    }
}
