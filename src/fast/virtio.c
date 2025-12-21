/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>
#include <stdint.h>

#include "src/fast/internal.h"
#include "src/include/fastpath.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"

const uint16_t vlan_tags[64] = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
    1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
    1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
    1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
};

static __rte_always_inline void virtio_tx(struct vhost_dev *dst_vdev, struct vhost_dev *src_vdev,
                                          struct rte_mbuf **pkts, uint16_t count);
static inline void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf **pkts, uint16_t count,
                                   struct mbuf_table *tx_q, uint16_t vlan_tag);
static void virtio_tx_offload(struct rte_mbuf *m);
static __rte_always_inline int virtio_tx_local(struct vhost_dev *vdev, struct rte_mbuf **pkts, uint16_t count);

// receive packets from VM's TX queue, route them to the correct destination
void poll_virtio_tx(struct vhost_dev *vdev, struct dataplane_context *ctx) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;

    if (unlikely(check_device_state(vdev, "poll_virtio_tx") != 0))
        return;

    // copy pkt from guest vring buffer to DPDK mbuf (vm -> dpdk)
    // This can fail if the vhost connection is broken
    count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, ctx->net.pool, pkts, MAX_PKT_BURST);

    if (unlikely((int16_t)count < 0)) {
        LOG_ERROR("Error: rte_vhost_dequeue_burst failed for vid=%d (device may be disconnected)\n", vdev->vid);
        vdev->remove = 1; // Mark device for removal
        return;
    }

    if (count > 0) {
        LOG_VM_IN("[vid=%d] Received %d packets from VM's TX queue\n", vdev->vid, count);
        PRINT_PKTS(pkts, count, LOG_VM_IN);
    }

    /* setup VMDq for the first packet */
    if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) { // device in MAC learning
        LOG_INFO("[vid=%d] In MAC learning mode, processing first packet\n", vdev->vid);
        if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1) { // failed to learn MAC from first packet
            LOG_ERROR("[vid=%d] MAC learning failed, dropping %d packets\n", vdev->vid, count);
            free_pkts(pkts, count);
            return; // Early return after freeing packets
        }
        LOG_INFO("[vid=%d] MAC learning successful, device now in RX mode\n", vdev->vid);
    }

    virtio_tx_route(vdev, pkts, count, &ctx->vhost.tx_q, vlan_tags[vdev->vid]);
}

static inline void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf **pkts, uint16_t count,
                                   struct mbuf_table *tx_q, uint16_t vlan_tag) {
    struct rte_mbuf *broadcast_pkts[MAX_PKT_BURST];
    struct rte_mbuf *external_pkts[MAX_PKT_BURST];
    struct rte_mbuf *local_pkts[MAX_PKT_BURST];
    struct rte_mbuf *tap_pkts[MAX_PKT_BURST];
    uint16_t broadcast_count = 0;
    uint16_t external_count = 0;
    uint16_t local_count = 0;
    uint16_t tap_count = 0;

    for (int i = 0; i < count; i++) {
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
        // Intercept ARP requests for the gateway (vhost-switch acts as gateway)
        if (unlikely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP))) {
            LOG_INFO("(%d) TX: ARP packet received. Processing...\n", vdev->vid);
            if (process_arp(vdev, pkts[i]) == 0) {
                continue;
            }
            LOG_INFO("(%d) TX: Broadcasting ARP to other VMs\n", vdev->vid);
        }

        if (unlikely(rte_is_broadcast_ether_addr(&eth_hdr->d_addr))) {
            // broadcast is sent first, then external (it will free pkts)
            broadcast_pkts[broadcast_count++] = pkts[i];
            external_pkts[external_count++] = rte_pktmbuf_clone(pkts[i], pkts[i]->pool);
            continue;
        }

        if (memcmp(&eth_hdr->d_addr, &config.mac, sizeof(struct rte_ether_addr)) == 0) {
            // Check if destination IP is gateway IP (for collector on host)
            if (likely(eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))) {
                struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
                uint32_t dst_ip = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
                if (unlikely(dst_ip == config.ip)) {
                    // Packet destined for gateway IP - forward to TAP (host network stack via br0)
                    LOG_INFO("(%d) TX: Packet destined for gateway IP %u.%u.%u.%u -> forwarding to TAP\n", vdev->vid,
                             (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff, (dst_ip >> 8) & 0xff, dst_ip & 0xff);
                    tap_pkts[tap_count++] = pkts[i];
                    continue;
                }
            }
            LOG_INFO("(%d) TX: MAC address is external\n", vdev->vid);
            external_pkts[external_count++] = pkts[i];
            continue;
        }

        local_pkts[local_count++] = pkts[i];
    }

    // broadcast packets
    if (unlikely(broadcast_count > 0)) {
        struct vhost_dev *vdev2;
        // TODOZ: Pre-compute broadcast list, use single loop
        for (int j = 0; j < fp_cores_max; j++) {
            struct dataplane_context *ctx = ctxs[j];
            for (int k = 0; k < ctx->vhost.device_num; k++) {
                vdev2 = ctx->vhost.vdev_list[k];
                if (vdev2 != NULL && vdev2 != vdev) {
                    struct rte_mbuf *clone_pkts[broadcast_count];
                    uint16_t clone_count = 0;
                    for (int l = 0; l < broadcast_count; l++) {
                        struct rte_mbuf *clone_pkt = rte_pktmbuf_clone(broadcast_pkts[l], broadcast_pkts[l]->pool);
                        if (unlikely(clone_pkt == NULL)) {
                            LOG_WARN("Failed to clone packet for broadcast to vid=%d\n", vdev2->vid);
                            continue;
                        }
                        clone_pkts[clone_count++] = clone_pkt;
                    }
                    virtio_tx(vdev2, vdev, clone_pkts, clone_count);
                }
            }
        }
    }

    // send to NIC
    // uint32_t nat_ip = (128 << 24) | (110 << 16) | (219 << 8) | 130; // 128.110.219.130
    // nat_translate_outbound(m, vdev->vid, nat_ip);
    struct rte_ether_hdr *eth_hdr;
    for (int i = 0; i < external_count; i++) {
        eth_hdr = rte_pktmbuf_mtod(external_pkts[i], struct rte_ether_hdr *);
        if (unlikely(eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))) {
            external_pkts[i]->ol_flags |= PKT_TX_VLAN_PKT; // offload flag indicating NIC should insert VLAN tag
            external_pkts[i]->vlan_tci = vlan_tag;         // Tag Control Information
        }

        if (external_pkts[i]->ol_flags & PKT_TX_TCP_SEG) // if TCP segmentation offload is enabled
            virtio_tx_offload(external_pkts[i]);         // prepare checksum offloads

        // Add packet to the TX queue's mbuf table
        tx_q->m_table[tx_q->len++] = external_pkts[i];
        if (config.enable_stats) {
            vdev->stats.tx_total++;
            vdev->stats.tx++;
        }
        if (unlikely(tx_q->len == MAX_PKT_BURST)) // if the queue is full
            flush_eth_tx(tx_q);                   // drain the queue (send packets to NIC)
    }

    if (unlikely(tx_q->len == MAX_PKT_BURST)) // if the queue is full
        flush_eth_tx(tx_q);                   // drain the queue (send packets to NIC)

    // // send to TAP (host network stack via br0)
    // if (tap_count > 0) {
    //     int sent = tap_tx_burst(tap_pkts, tap_count);
    //     LOG_INFO("(%d) Forwarded %d/%d packets to TAP interface\n", vdev->vid, sent, tap_count);
    // }

    // send to local VM
    if (local_count > 0) {
        virtio_tx_local(vdev, local_pkts, local_count);
    }
}

// Transmits a packet to vhost device via virtqueue.
static __rte_always_inline void virtio_tx(struct vhost_dev *dst_vdev, struct vhost_dev *src_vdev,
                                          struct rte_mbuf **pkts, uint16_t count) {
    uint16_t ret;

    if (unlikely(check_device_state(dst_vdev, "virtio_tx") != 0)) {
        free_pkts(pkts, count);
        return;
    }

    ret = rte_vhost_enqueue_burst(dst_vdev->vid, VIRTIO_RXQ, pkts, count);
    free_pkts(pkts, count);

    LOG_VM_OUT("(%d) Sent packet to vid=%d\n", src_vdev->vid, dst_vdev->vid);
    PRINT_PKTS(pkts, count, LOG_VM_OUT);

    if (unlikely(ret == 0)) {
        // Rate-limited logging: log once per second with cumulative count
        uint64_t now = rte_get_tsc_cycles();
        uint64_t log_interval_tsc = rte_get_tsc_hz(); // 1 second in TSC cycles

        // Initialize on first failure
        if (unlikely(dst_vdev->last_failed_log_ts == 0)) {
            dst_vdev->last_failed_log_ts = now;
            dst_vdev->failed_pkts_count = 0;
        }

        // Accumulate failed packets
        dst_vdev->failed_pkts_count += count;

        // Log once per second
        if (unlikely(now - dst_vdev->last_failed_log_ts >= log_interval_tsc)) {
            LOG_WARN("(%d) Failed to enqueue %lu cumulative packets to vid=%d (over last second)\n", src_vdev->vid,
                     dst_vdev->failed_pkts_count, dst_vdev->vid);
            dst_vdev->last_failed_log_ts = now;
            dst_vdev->failed_pkts_count = 0;
        }
        return;
    }

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
static __rte_always_inline int virtio_tx_local(struct vhost_dev *vdev, struct rte_mbuf **pkts, uint16_t count) {
    struct rte_ether_hdr *pkt_hdr;
    struct vhost_dev *dst_vdev;

    if (unlikely(check_device_state(vdev, "virtio_tx_local") != 0))
        return -1;

    // assume in 1 poll from vhost, all local pkts are for same dest vm
    pkt_hdr = rte_pktmbuf_mtod(pkts[0], struct rte_ether_hdr *);

    // must search all cores (vms can be on different cores)
    dst_vdev = find_vhost_dev(&pkt_hdr->d_addr);
    if (dst_vdev == NULL) {
        LOG_WARN("(%d) TX: Destination MAC address not found. Dropping packet.\n", vdev->vid);
        PRINT_PKTS_WARN(&pkts[0], 1, LOG_WARN);
        return -1;
    }

    if (vdev->vid == dst_vdev->vid) {
        LOG_INFO("(%d) TX: src and dst MAC is same. Dropping packet.\n", vdev->vid);
        return 0;
    }

    LOG_INFO("(%d) TX: MAC address is local\n", dst_vdev->vid);
    virtio_tx(dst_vdev, vdev, pkts, count);
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
