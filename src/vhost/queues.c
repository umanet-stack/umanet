/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/config/config.h"
#include "src/eth/eth.h"
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
        RTE_LOG(ERR, VHOST_DATA, "(%d) device is using a registered MAC!\n", vdev->vid);
        return -1;
    }

    // copy MAC from pkt to dev
    for (i = 0; i < RTE_ETHER_ADDR_LEN; i++)
        vdev->mac_address.addr_bytes[i] = pkt_hdr->s_addr.addr_bytes[i];

    /* vlan_tag currently uses the device_id. */
    // device 0 → VLAN 1000
    vdev->vlan_tag = eth.vlan_tags[vdev->vid];

    /* Print out VMDQ registration info. */
    RTE_LOG(INFO, VHOST_DATA, "(%d) mac %02x:%02x:%02x:%02x:%02x:%02x and vlan %d registered\n", vdev->vid,
            vdev->mac_address.addr_bytes[0], vdev->mac_address.addr_bytes[1], vdev->mac_address.addr_bytes[2],
            vdev->mac_address.addr_bytes[3], vdev->mac_address.addr_bytes[4], vdev->mac_address.addr_bytes[5],
            vdev->vlan_tag);

    /* Register the MAC address without pool */
    ret = rte_eth_dev_mac_addr_add(config.ports[0], &vdev->mac_address, 0);
    if (ret)
        RTE_LOG(ERR, VHOST_DATA, "(%d) failed to add device MAC address\n", vdev->vid);

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
        rte_eth_dev_mac_addr_remove(config.ports[0], &vdev->mac_address);
        for (i = 0; i < 6; i++)
            vdev->mac_address.addr_bytes[i] = 0;

        vdev->vlan_tag = 0;

        /*Clear out the receive buffers*/
        rx_count = rte_eth_rx_burst(config.ports[0], (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);

        while (rx_count) {                 // until queue is empty
            for (i = 0; i < rx_count; i++) // Frees each packet buffer back to mbuf pool
                rte_pktmbuf_free(pkts_burst[i]);

            // Receives next batch of packets from queue
            rx_count = rte_eth_rx_burst(config.ports[0], (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);
        }

        vdev->ready = DEVICE_MAC_LEARNING;
    }
}

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
        RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) TX: src and dst MAC is same. Dropping packet.\n", vdev->vid);
        return 0;
    }

    RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) TX: MAC address is local\n", dst_vdev->vid);

    if (unlikely(dst_vdev->remove)) {
        RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) device is marked for removal\n", dst_vdev->vid);
        return 0;
    }

    virtio_xmit(dst_vdev, vdev, m);
    return 0;
}

/*
 * Check if the destination MAC of a packet is one local VM,
 * and get its vlan tag, and offset if it is.
 */
static __rte_always_inline int find_local_dest(struct vhost_dev *vdev, struct rte_mbuf *m, uint32_t *offset,
                                               uint16_t *vlan_tag) {
    struct vhost_dev *dst_vdev;
    struct rte_ether_hdr *pkt_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    dst_vdev = find_vhost_dev(&pkt_hdr->d_addr);
    if (!dst_vdev)
        return 0;

    if (vdev->vid == dst_vdev->vid) {
        RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) TX: src and dst MAC is same. Dropping packet.\n", vdev->vid);
        return -1;
    }

    /*
     * HW vlan strip will reduce the packet length
     * by minus length of vlan tag, so need restore
     * the packet length by plus it.
     */
    *offset = VLAN_HLEN;
    *vlan_tag = eth.vlan_tags[vdev->vid];

    RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) TX: pkt to local VM device id: (%d), vlan tag: %u.\n", vdev->vid, dst_vdev->vid,
               *vlan_tag);

    return 0;
}

static uint16_t
// pseudo header checksum
get_psd_sum(void *l3_hdr, uint64_t ol_flags) {
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

void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
void do_drain_mbuf_table(struct mbuf_table *tx_q) {
    uint16_t count;

    count = rte_eth_tx_burst(config.ports[0], tx_q->txq_id, tx_q->m_table, tx_q->len);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}

/*
 * This function routes the TX packet to the correct interface. This
 * may be a local device or the physical port.
 */
// determines where to send packet from VM to NIC or another VM
void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, uint16_t vlan_tag) {
    struct mbuf_table *tx_q;
    unsigned offset = 0;
    const uint16_t lcore_id = rte_lcore_id(); // current core (each core has its own tx queue)
    struct rte_ether_hdr *nh;

    nh = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);         // get the Ethernet header
    if (unlikely(rte_is_broadcast_ether_addr(&nh->d_addr))) { // if dest MAC is broadcast
        struct vhost_dev *vdev2;

        TAILQ_FOREACH(vdev2, &vhost.vhost_dev_list, global_vdev_entry) { // iterate over all vhost devices
            if (vdev2 != vdev)                                           // if not the same device
                virtio_xmit(vdev2, vdev, m);
        }
        goto queue2nic; // also go to NIC
    }

    /*check if destination is local VM (same host)*/
    if ((config.vm2vm_mode == VM2VM_SOFTWARE) && (virtio_tx_local(vdev, m) == 0)) {
        rte_pktmbuf_free(m); //  If delivered locally, free the mbuf (no need to send to NIC)
        return;
    }

    if (unlikely(config.vm2vm_mode == VM2VM_HARDWARE)) { // uses NIC hardware for VM switching via VLANs
        if (unlikely(find_local_dest(vdev, m, &offset, &vlan_tag) != 0)) {
            // destination lookup fails (non-zero return)
            rte_pktmbuf_free(m);
            return;
        }
    }

    RTE_LOG_DP(DEBUG, VHOST_DATA, "(%d) TX: MAC address is external\n", vdev->vid);
    // sending to NIC

queue2nic:

    /*Add packet to the port tx queue*/
    tx_q = &vhost.lcore_tx_queue[lcore_id];

    nh = rte_pktmbuf_mtod(
        m, struct rte_ether_hdr *); // Re-extract Ethernet header (might have been modified in VM2VM processing)
    if (unlikely(nh->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN))) {
        // if packet already has a VLAN tag
        /* Guest has inserted the vlan tag. */
        struct rte_vlan_hdr *vh = (struct rte_vlan_hdr *)(nh + 1); // VLAN header
        uint16_t vlan_tag_be = rte_cpu_to_be_16(vlan_tag);         // Convert VLAN tag to big-endian
        if ((config.vm2vm_mode == VM2VM_HARDWARE) && (vh->vlan_tci != vlan_tag_be))
            vh->vlan_tci = vlan_tag_be;
    } else {                            // packet doesn't have VLAN tag yet
        m->ol_flags |= PKT_TX_VLAN_PKT; // offload flag indicating NIC should insert VLAN tag

        /*
         * Find the right seg to adjust the data len when offset is
         * bigger than tail room size.
         */
        if (unlikely(config.vm2vm_mode == VM2VM_HARDWARE)) {
            if (likely(offset <= rte_pktmbuf_tailroom(m)))
                m->data_len += offset;
            else {
                struct rte_mbuf *seg = m;

                while ((seg->next != NULL) && (offset > rte_pktmbuf_tailroom(seg)))
                    seg = seg->next;

                seg->data_len += offset;
            }
            m->pkt_len += offset;
        }

        m->vlan_tci = vlan_tag; // Tag Control Information
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
        do_drain_mbuf_table(tx_q);            // drain the queue (send packets to NIC)
}

/*
 * While creating an mbuf pool, one key thing is to figure out how
 * many mbuf entries is enough for our use. FYI, here are some
 * guidelines:
 *
 * - Each rx queue would reserve @nr_rx_desc mbufs at queue setup stage
 *
 * - For each switch core (A CPU core does the packet switch), we need
 *   also make some reservation for receiving the packets from virtio
 *   Tx queue. How many is enough depends on the usage. It's normally
 *   a simple calculation like following:
 *
 *       MAX_PKT_BURST * max packet size / mbuf size
 *
 *   So, we definitely need allocate more mbufs when TSO is enabled.
 *
 * - Similarly, for each switching core, we should serve @nr_rx_desc
 *   mbufs for receiving the packets from physical NIC device.
 *
 * - We also need make sure, for each switch core, we have allocated
 *   enough mbufs to fill up the mbuf cache.
 */
// if MAX_PKT_BURST=32, max packet=9KB, mbuf=2KB: need 329/2=144 mbufs
// TSO (TCP Segmentation Offload) allows huge packets (64KB), so need many more mbufs
void create_mbuf_pool(uint16_t nr_port, uint32_t nr_switch_core, uint32_t mbuf_size, uint32_t nr_queues,
                      uint32_t nr_rx_desc, uint32_t nr_mbuf_cache) {
    // nr_queues: Total number of RX queues
    // nr_rx_desc: Number of RX descriptors per queue (ring size)

    uint32_t nr_mbufs;
    uint32_t nr_mbufs_per_core;
    uint32_t mtu = 1500; // Maximum Transmission Unit to standard Ethernet size (1500 bytes)

    if (config.mergeable)
        mtu = 9000;        // jumbo frames: allow single packet to span multiple mbufs
    if (config.enable_tso) // TSO allows sending huge packets that NIC splits into smaller segments
        mtu = 64 * 1024;   // 64KB (maximum TSO packet size)

    // how many mbufs needed for a full burst of max-sized packets
    nr_mbufs_per_core = (mtu + mbuf_size) * MAX_PKT_BURST / (mbuf_size - RTE_PKTMBUF_HEADROOM);
    nr_mbufs_per_core += nr_rx_desc;
    nr_mbufs_per_core = RTE_MAX(nr_mbufs_per_core, nr_mbuf_cache);

    nr_mbufs = nr_queues * nr_rx_desc; // Mbufs for all NIC RX queue descriptors
    nr_mbufs += nr_mbufs_per_core * nr_switch_core;
    nr_mbufs *= nr_port; // If multi-port setup, each port needs its own pool

    eth.mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", nr_mbufs, nr_mbuf_cache, 0, mbuf_size, rte_socket_id());
    if (eth.mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
}