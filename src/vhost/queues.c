/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/config/config.h"
#include "src/fast/network.h"
#include "src/include/tas.h"
#include "src/utils/utils.h"
#include "src/vhost/vhost.h"

/*
 * This function learns the MAC address of the device and registers this along with a
 * vlan tag to a VMDQ.
 */
int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m) {
    struct rte_ether_hdr *pkt_hdr;
    int i, ret;

    /* Learn MAC address of guest device from packet */
    pkt_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    if (find_vhost_dev(&pkt_hdr->s_addr)) {
        printf("(%d) device is using a registered MAC!\n", vdev->vid);
        return -1;
    }

    // copy MAC from pkt to dev
    for (i = 0; i < RTE_ETHER_ADDR_LEN; i++)
        vdev->mac_address.addr_bytes[i] = pkt_hdr->s_addr.addr_bytes[i];

    /* vlan_tag currently uses the device_id. */
    // device 0 → VLAN 1000
    vdev->vlan_tag = vlan_tags[vdev->vid];

    /* Print out VMDQ registration info. */
    printf("(%d) mac %02x:%02x:%02x:%02x:%02x:%02x and vlan %d registered\n", vdev->vid,
           vdev->mac_address.addr_bytes[0], vdev->mac_address.addr_bytes[1], vdev->mac_address.addr_bytes[2],
           vdev->mac_address.addr_bytes[3], vdev->mac_address.addr_bytes[4], vdev->mac_address.addr_bytes[5],
           vdev->vlan_tag);

    /* Register the MAC address without pool */
    ret = rte_eth_dev_mac_addr_add(net_port_id, &vdev->mac_address, 0);
    if (ret)
        printf("(%d) failed to add device MAC address\n", vdev->vid);

    /* Set device as ready for RX. */
    // Changes state from DEVICE_MAC_LEARNING to DEVICE_RX
    vdev->ready = DEVICE_RX;

    return 0;
}

/*
 * Removes MAC address and vlan tag from VMDQ. Ensures that nothing is adding buffers to the RX
 * queue before disabling RX on the device.
 */
void unlink_vmdq(struct vhost_dev *vdev) {
    unsigned i = 0;
    unsigned rx_count;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

    if (vdev->ready == DEVICE_RX) {
        /*clear MAC and VLAN settings*/
        rte_eth_dev_mac_addr_remove(net_port_id, &vdev->mac_address);
        for (i = 0; i < 6; i++)
            vdev->mac_address.addr_bytes[i] = 0;

        vdev->vlan_tag = 0;

        /*Clear out the receive buffers*/
        rx_count = rte_eth_rx_burst(net_port_id, (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);

        while (rx_count) {                 // until queue is empty
            for (i = 0; i < rx_count; i++) // Frees each packet buffer back to mbuf pool
                rte_pktmbuf_free(pkts_burst[i]);

            // Receives next batch of packets from queue
            rx_count = rte_eth_rx_burst(net_port_id, (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);
        }

        vdev->ready = DEVICE_MAC_LEARNING;
    }
}

// currently simulates sending pkts to vm
// Transmits a packet from one vhost device to another via virtqueue.
static __rte_always_inline void virtio_xmit(struct vhost_dev *dst_vdev, struct vhost_dev *src_vdev,
                                            struct rte_mbuf *m) {
    uint16_t ret;
    // dpdk to vm
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

    virtio_xmit(dst_vdev, vdev, m);
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

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
void flush_eth_tx(struct mbuf_table *tx_q) {
    uint16_t count;

    printf("do_drain_mbuf_table\n");
    printf("txq_id: %d\n", tx_q->txq_id);
    printf("len: %d\n", tx_q->len);
    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    printf("count: %d\n", count);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}

/*
 * This function routes the TX packet to the correct interface. This
 * may be a local device or the physical port.
 */
// determines where to send packet from VM to NIC or another VM
void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, struct mbuf_table *tx_q, uint16_t vlan_tag) {
    struct rte_ether_hdr *nh;

    nh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *); // get the Ethernet header
    if (unlikely(rte_is_broadcast_ether_addr(&nh->d_addr))) {
        struct vhost_dev *vdev2;

        for (int i = 0; i < fp_cores_max; i++) {
            struct dataplane_context *ctx = ctxs[i];
            for (int j = 0; j < ctx->vhost.device_num; j++) {
                vdev2 = ctx->vhost.vdev_list[j];
                if (vdev2 != NULL && vdev2 != vdev)
                    virtio_xmit(vdev2, vdev, m);
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
