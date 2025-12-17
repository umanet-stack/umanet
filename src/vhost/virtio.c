/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "src/include/fastpath.h"
#include "src/utils/utils.h"
#include "src/vhost/vhost.h"

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

static inline void print_pkts(struct rte_mbuf **pkts, uint16_t count);
static __rte_always_inline void virtio_tx(struct vhost_dev *dst_vdev, struct vhost_dev *src_vdev, struct rte_mbuf *m);
static inline void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, struct mbuf_table *tx_q,
                                   uint16_t vlan_tag);
static void virtio_tx_offload(struct rte_mbuf *m);
static __rte_always_inline int virtio_tx_local(struct vhost_dev *vdev, struct rte_mbuf *m);

// receive packets from VM's TX queue, route them to the correct destination
void poll_virtio_tx(struct vhost_dev *vdev, struct dataplane_context *ctx) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    // copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
    count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, ctx->net.pool, pkts, MAX_PKT_BURST);

    if (count > 0) {
        printf("[Device vid=%d state=%d] Received %d packets from VM's TX queue\n", vdev->vid, vdev->ready, count);
    }
    print_pkts(pkts, count);

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

    for (i = 0; i < count; ++i) {
        virtio_tx_route(vdev, pkts[i], &ctx->vhost.tx_q, vlan_tags[vdev->vid]);
    }
}

static inline void print_pkts(struct rte_mbuf **pkts, uint16_t count) {
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
}

// determines whether to send packet from VM to NIC or local VM
static inline void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, struct mbuf_table *tx_q,
                                   uint16_t vlan_tag) {
    struct rte_ether_hdr *nh;

    nh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *); // get the Ethernet header
    if (unlikely(rte_is_broadcast_ether_addr(&nh->d_addr))) {
        struct vhost_dev *vdev2;

        for (int i = 0; i < fp_cores_max; i++) {
            struct dataplane_context *ctx = ctxs[i];
            for (int j = 0; j < ctx->vhost.device_num; j++) {
                vdev2 = ctx->vhost.vdev_list[j];
                if (vdev2 != NULL && vdev2 != vdev)
                    virtio_tx(vdev2, vdev, m);
            }
        }
        goto queue2nic;
    }

    /*check if destination is local VM (same host)*/
    if (virtio_tx_local(vdev, m) == 0) {
        rte_pktmbuf_free(m); //  If delivered locally, free the mbuf (no need to send to NIC)
        return;
    }

    printf("(%d) TX: MAC address is external\n", vdev->vid);
    // sending to NIC

queue2nic:
    nh = rte_pktmbuf_mtod(
        m, struct rte_ether_hdr *); // Re-extract Ethernet header (might have been modified in VM2VM processing)
    if (unlikely(nh->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))) {
        // packet doesn't have VLAN tag yet
        m->ol_flags |= PKT_TX_VLAN_PKT; // offload flag indicating NIC should insert VLAN tag
        m->vlan_tci = vlan_tag;         // Tag Control Information
    }

    if (m->ol_flags & PKT_TX_TCP_SEG) // if TCP segmentation offload is enabled
        virtio_tx_offload(m);         // prepare checksum offloads

    // Add packet to the TX queue's mbuf table
    tx_q->m_table[tx_q->len++] = m;
    if (config.enable_stats) {
        vdev->stats.tx_total++;
        vdev->stats.tx++;
    }

    if (unlikely(tx_q->len == MAX_PKT_BURST)) // if the queue is full
        flush_eth_tx(tx_q);                   // drain the queue (send packets to NIC)
}

// Transmits a packet to vhost device via virtqueue.
static __rte_always_inline void virtio_tx(struct vhost_dev *dst_vdev, struct vhost_dev *src_vdev, struct rte_mbuf *m) {
    uint16_t ret;
    ret = rte_vhost_enqueue_burst(dst_vdev->vid, VIRTIO_RXQ, &m, 1);

    // dest stats use atomic operations (multiple cores may write)
    // source stats don't (single core writes)
    if (config.enable_stats) {
        rte_atomic64_inc(&dst_vdev->stats.rx_total_atomic);
        rte_atomic64_add(&dst_vdev->stats.rx_atomic, ret);
        src_vdev->stats.tx_total++;
        src_vdev->stats.tx += ret;
    }
}

/*
 * Check if the packet destination MAC address is for a local (same host) device. If so then put
 * the packet on that devices RX queue. If not then return.
 */
static __rte_always_inline int virtio_tx_local(struct vhost_dev *vdev, struct rte_mbuf *m) {
    struct rte_ether_hdr *pkt_hdr;
    struct vhost_dev *dst_vdev;

    pkt_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    dst_vdev = find_vhost_dev(&pkt_hdr->d_addr);
    if (!dst_vdev)
        return -1;

    if (vdev->vid == dst_vdev->vid) {
        printf("(%d) TX: src and dst MAC is same. Dropping packet.\n", vdev->vid);
        return 0;
    }

    printf("(%d) TX: MAC address is local\n", dst_vdev->vid);

    if (unlikely(dst_vdev->remove)) {
        printf("(%d) device is marked for removal\n", dst_vdev->vid);
        return 0;
    }

    virtio_tx(dst_vdev, vdev, m);
    return 0;
}

// pseudo header checksum
static uint16_t get_psd_sum(void *l3_hdr, uint64_t ol_flags) {
    if (ol_flags & PKT_TX_IPV4)
        return rte_ipv4_phdr_cksum(l3_hdr, ol_flags);
    else /* assume ethertype == RTE_ETHER_TYPE_IPV6 */
        return rte_ipv6_phdr_cksum(l3_hdr, ol_flags);
}

// prepare checksum offloads
static void virtio_tx_offload(struct rte_mbuf *m) {
    void *l3_hdr;
    struct rte_ipv4_hdr *ipv4_hdr = NULL;
    struct rte_tcp_hdr *tcp_hdr = NULL;
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    // l3 header position
    l3_hdr = (char *)eth_hdr + m->l2_len;

    if (m->ol_flags & PKT_TX_IPV4) {
        ipv4_hdr = l3_hdr;
        ipv4_hdr->hdr_checksum = 0;     // hw will calculate checksum
        m->ol_flags |= PKT_TX_IP_CKSUM; // tell NIC hw to compute checksum
    }

    // l4 header position
    tcp_hdr = (struct rte_tcp_hdr *)((char *)l3_hdr + m->l3_len);
    // hardware will complete the full TCP checksum calculation
    tcp_hdr->cksum = get_psd_sum(l3_hdr, m->ol_flags);
}
