/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <signal.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/param.h>
#include <unistd.h>

#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_pause.h>
#include <rte_string_fns.h>
#include <rte_tcp.h>
#include <rte_vhost.h>

#include "main.h"
#include "src/config/config.h"
#include "src/eth/eth.h"
#include "src/tcp_state.h"
#include "src/vhost/vhost.h"
#include "tcp_fastpath.h"

#ifndef MAX_QUEUES
#define MAX_QUEUES 128
#endif

/* the maximum number of external ports supported */
#define MAX_SUP_PORTS 1

#define MBUF_CACHE_SIZE 128
#define MBUF_DATA_SIZE RTE_MBUF_DEFAULT_BUF_SIZE

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024

static unsigned lcore_ids[RTE_MAX_LCORE];

/* Used for queueing bursts of TX packets. */
struct mbuf_table {
    unsigned len;
    unsigned txq_id;
    struct rte_mbuf *m_table[MAX_PKT_BURST];
};

/* TX queue for each data core. */
struct mbuf_table lcore_tx_queue[RTE_MAX_LCORE];

#define MBUF_TABLE_DRAIN_TSC ((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US)
#define VLAN_HLEN 4

static __rte_always_inline struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac) {
    struct vhost_dev *vdev;

    TAILQ_FOREACH(vdev, &vhost.vhost_dev_list, global_vdev_entry) {
        if (vdev->ready == DEVICE_RX && rte_is_same_ether_addr(mac, &vdev->mac_address))
            return vdev;
    }

    return NULL;
}

/*
 * This function learns the MAC address of the device and registers this along with a
 * vlan tag to a VMDQ.
 */
static int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m) {
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

    /* Register the MAC address. */
    if (eth.vmdq_enabled) {
        ret = rte_eth_dev_mac_addr_add(config.ports[0], &vdev->mac_address, (uint32_t)vdev->vid + eth.vmdq_pool_base);
        if (ret)
            RTE_LOG(ERR, VHOST_DATA, "(%d) failed to add device MAC address to VMDQ\n", vdev->vid);
        rte_eth_dev_set_vlan_strip_on_queue(config.ports[0], vdev->vmdq_rx_q, 1);
    } else {
        /* In non-VMDq mode, just register MAC address without pool */
        ret = rte_eth_dev_mac_addr_add(config.ports[0], &vdev->mac_address, 0);
        if (ret)
            RTE_LOG(ERR, VHOST_DATA, "(%d) failed to add device MAC address\n", vdev->vid);
    }

    /* Set device as ready for RX. */
    // Changes state from DEVICE_MAC_LEARNING to DEVICE_RX
    vdev->ready = DEVICE_RX;

    return 0;
}

/*
 * Removes MAC address and vlan tag from VMDQ. Ensures that nothing is adding buffers to the RX
 * queue before disabling RX on the device.
 */
static inline void unlink_vmdq(struct vhost_dev *vdev) {
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

    if (config.builtin_net_driver) {
        // dpdk to vm
        ret = vs_enqueue_pkts(dst_vdev, VIRTIO_RXQ, &m, 1);
    } else {
        ret = rte_vhost_enqueue_burst(dst_vdev->vid, VIRTIO_RXQ, &m, 1);
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

static inline void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
static __rte_always_inline void do_drain_mbuf_table(struct mbuf_table *tx_q) {
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
static __rte_always_inline void virtio_tx_route(struct vhost_dev *vdev, struct rte_mbuf *m, uint16_t vlan_tag) {
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
    tx_q = &lcore_tx_queue[lcore_id];

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

static __rte_always_inline void drain_mbuf_table(struct mbuf_table *tx_q) {
    // static = function-scope, keeps value between function calls
    static uint64_t prev_tsc; // previous timestamp
    uint64_t cur_tsc;

    if (tx_q->len == 0)
        return;

    cur_tsc = rte_rdtsc(); // current timestamp
    if (unlikely(cur_tsc - prev_tsc > MBUF_TABLE_DRAIN_TSC)) {
        // time elapsed since last drain exceeds threshold
        prev_tsc = cur_tsc;

        RTE_LOG_DP(DEBUG, VHOST_DATA, "TX queue drained after timeout with burst size %u\n", tx_q->len);
        do_drain_mbuf_table(tx_q);
    }
}

// receive packets from physical NIC and forward them to a VM
static __rte_always_inline void drain_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];

    // receive packets from physical NIC
    rx_count = rte_eth_rx_burst(config.ports[0], vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;

    if (config.builtin_net_driver) {
        // send packets to guest virtio RX ring
        enqueue_count = vs_enqueue_pkts(vdev, VIRTIO_RXQ, pkts, rx_count);
    } else {
        enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);
    }

    /* Retry if necessary */
    if (config.enable_retry && unlikely(enqueue_count < rx_count)) {
        uint32_t retry = 0;

        while (enqueue_count < rx_count && retry++ < config.burst_rx_retry_num) { // max 4 retries
            rte_delay_us(config.burst_rx_delay_time);
            if (config.builtin_net_driver) {
                enqueue_count += vs_enqueue_pkts(vdev, VIRTIO_RXQ, &pkts[enqueue_count], rx_count - enqueue_count);
            } else {
                enqueue_count +=
                    rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &pkts[enqueue_count], rx_count - enqueue_count);
            }
        }
    }

    if (config.enable_stats) {
        rte_atomic64_add(&vdev->stats.rx_total_atomic, rx_count);
        rte_atomic64_add(&vdev->stats.rx_atomic, enqueue_count);
    }

    free_pkts(pkts, rx_count);
}

// receive packets from VM's TX queue, route them to the correct destination
static __rte_always_inline void drain_virtio_tx(struct vhost_dev *vdev) {
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    uint16_t count;
    uint16_t i;

    if (config.builtin_net_driver) {
        // copy pkt from guest vring buffer to DPDK mbuf
        count = vs_dequeue_pkts(vdev, VIRTIO_TXQ, eth.mbuf_pool, pkts, MAX_PKT_BURST);
    } else {
        count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, eth.mbuf_pool, pkts, MAX_PKT_BURST);
    }

    /* setup VMDq for the first packet */
    if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) { // device in MAC learning
        if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1)      // failed to learn MAC from first packet
            free_pkts(pkts, count);
    }

    for (i = 0; i < count; ++i) { // loop received packets
        // NEW: Try TCP classification
        uint32_t sip, dip;
        uint16_t sport, dport;
        struct tcp_flow_state *flow = NULL;

        if (vdev->tcp_offload_enabled && tcp_parse_packet(pkts[i], &sip, &dip, &sport, &dport) == 0) {
            // Lookup existing flow
            flow = tcp_flow_lookup(sip, dip, sport, dport);

            if (!flow) {
                // Check if SYN packet
                struct rte_tcp_hdr *tcp = rte_pktmbuf_mtod(pkts[i], struct rte_tcp_hdr *);
                if (tcp->tcp_flags & RTE_TCP_SYN_FLAG) {
                    flow = tcp_flow_create(sip, dip, sport, dport, vdev->vid);
                }
            }
            if (flow) {
                RTE_LOG_DP(DEBUG, VHOST_DATA, "Packet belongs to tracked flow\n");
                // tcp_flow_update(flow, pkts[i], 1 /* inbound */);
                // Still forward via L2 for now
            }
        }
        virtio_tx_route(vdev, pkts[i], eth.vlan_tags[vdev->vid]); // route each to correct destination
    }
}

/*
 * Main function of vhost-switch. It basically does:
 *
 * for each vhost device {
 *    - drain_eth_rx()
 *
 *      Which drains the host eth Rx queue linked to the vhost device,
 *      and deliver all of them to guest virito Rx ring associated with
 *      this vhost device.
 *
 *    - drain_virtio_tx()
 *
 *      Which drains the guest virtio Tx queue and deliver all of them
 *      to the target, which could be another vhost device, or the
 *      physical eth dev. The route is done in function "virtio_tx_route".
 * }
 */
static int switch_worker(void *arg __rte_unused) {
    unsigned i;
    unsigned lcore_id = rte_lcore_id();
    struct vhost_dev *vdev;
    struct mbuf_table *tx_q;

    RTE_LOG(INFO, VHOST_DATA, "Procesing on Core %u started\n", lcore_id);

    tx_q = &lcore_tx_queue[lcore_id];
    // Get pointer to this core's TX queue
    for (i = 0; i < rte_lcore_count(); i++) {
        if (lcore_ids[i] == lcore_id) {
            tx_q->txq_id = i;
            break;
        }
    }

    while (1) {
        drain_mbuf_table(tx_q); // drain if timeout has elapsed

        /*
         * Inform the configuration core that we have exited the
         * linked list and that no devices are in use if requested.
         */
        if (vhost.lcore_info[lcore_id].dev_removal_flag == REQUEST_DEV_REMOVAL)
            vhost.lcore_info[lcore_id].dev_removal_flag = ACK_DEV_REMOVAL;

        /*
         * Process vhost devices
         */
        TAILQ_FOREACH(vdev, &vhost.lcore_info[lcore_id].vdev_list, lcore_vdev_entry) {
            if (unlikely(vdev->remove)) { // device is marked for removal
                unlink_vmdq(vdev);
                vdev->ready = DEVICE_SAFE_REMOVE;
                continue;
            }

            if (likely(vdev->ready == DEVICE_RX))
                drain_eth_rx(vdev); // receive packets from physical NIC and forward them to a VM

            if (likely(!vdev->remove)) // device is not being removed (double-check)
                drain_virtio_tx(vdev); // receive packets from VM's TX queue, route them to the correct destination
        }
    }

    return 0;
}

/*
 * This is a thread will wake up after a period to print stats if the user has
 * enabled them.
 */
static void *print_stats(__rte_unused void *arg) {
    struct vhost_dev *vdev;
    uint64_t tx_dropped, rx_dropped;
    uint64_t tx, tx_total, rx, rx_total;
    const char clr[] = {27, '[', '2', 'J', '\0'};
    const char top_left[] = {27, '[', '1', ';', '1', 'H', '\0'};

    while (1) {
        sleep(config.enable_stats);

        /* Clear screen and move to top left */
        printf("%s%s\n", clr, top_left);
        printf("Device statistics =================================\n");

        TAILQ_FOREACH(vdev, &vhost.vhost_dev_list, global_vdev_entry) {
            tx_total = vdev->stats.tx_total;
            tx = vdev->stats.tx;
            tx_dropped = tx_total - tx;

            rx_total = rte_atomic64_read(&vdev->stats.rx_total_atomic);
            rx = rte_atomic64_read(&vdev->stats.rx_atomic);
            rx_dropped = rx_total - rx;

            printf("Statistics for device %d\n"
                   "-----------------------\n"
                   "TX total:              %" PRIu64 "\n"
                   "TX dropped:            %" PRIu64 "\n"
                   "TX successful:         %" PRIu64 "\n"
                   "RX total:              %" PRIu64 "\n"
                   "RX dropped:            %" PRIu64 "\n"
                   "RX successful:         %" PRIu64 "\n",
                   vdev->vid, tx_total, tx_dropped, tx, rx_total, rx_dropped, rx);
        }

        printf("===================================================\n");

        fflush(stdout);
    }

    return NULL;
}

static void unregister_drivers(int socket_num) {
    int i, ret;

    for (i = 0; i < socket_num; i++) {
        // each path is PATH_MAX bytes apart
        ret = rte_vhost_driver_unregister(config.socket_files + i * PATH_MAX);
        if (ret != 0)
            RTE_LOG(ERR, VHOST_CONFIG, "Fail to unregister vhost driver for %s.\n", config.socket_files + i * PATH_MAX);
    }
}

/* When we receive a INT signal, unregister vhost driver */
static void sigint_handler(__rte_unused int signum) {
    /* Unregister vhost driver. */
    unregister_drivers(config.nb_sockets);

    exit(0);
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
static void create_mbuf_pool(uint16_t nr_port, uint32_t nr_switch_core, uint32_t mbuf_size, uint32_t nr_queues,
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

/*
 * Main function, does initialisation and calls the per-lcore functions.
 */
int main(int argc, char *argv[]) {
    unsigned lcore_id, core_id = 0;
    unsigned nb_ports, valid_num_ports;
    int ret, i;
    uint16_t portid;
    static pthread_t tid;
    uint64_t flags = 0;

    // Register signal handler for SIGINT (Ctrl+C) (graceful shutdown)
    signal(SIGINT, sigint_handler);

    /* init EAL (Environment Abstraction Layer) */
    ret = rte_eal_init(argc, argv); // Parses DPDK-specific arguments (--lcores, --huge-dir, etc.)
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    argc -= ret; // Update argc to exclude DPDK-specific arguments
    argv += ret;

    /* parse app arguments */
    ret = us_vhost_parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid argument\n");

    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        TAILQ_INIT(&vhost.lcore_info[lcore_id].vdev_list); // init first,last dev list

        if (rte_lcore_is_enabled(lcore_id))
            lcore_ids[core_id++] = lcore_id;
    }

    if (rte_lcore_count() > RTE_MAX_LCORE)
        rte_exit(EXIT_FAILURE, "Not enough cores\n");

    /* Get the number of physical ports. */
    nb_ports = rte_eth_dev_count_avail(); // Count available (not disabled) Ethernet ports (physical NICs)

    /*
     * Update the global var NUM_PORTS and global array PORTS
     * and get value of var VALID_NUM_PORTS according to system ports number
     */
    valid_num_ports = check_ports_num(nb_ports);

    if ((valid_num_ports == 0) || (valid_num_ports > MAX_SUP_PORTS)) {
        RTE_LOG(INFO, VHOST_PORT,
                "Current enabled port number is %u,"
                "but only %u port can be enabled\n",
                config.num_ports, MAX_SUP_PORTS);
        return -1;
    }

    /*
     * FIXME: here we are trying to allocate mbufs big enough for
     * @MAX_QUEUES, but the truth is we're never going to use that
     * many queues here. We probably should only do allocation for
     * those queues we are going to use.
     */
    // number of worker cores (minus master core)
    create_mbuf_pool(valid_num_ports, rte_lcore_count() - 1, MBUF_DATA_SIZE, MAX_QUEUES, RTE_TEST_RX_DESC_DEFAULT,
                     MBUF_CACHE_SIZE);

    if (config.vm2vm_mode == VM2VM_HARDWARE) {
        /* Enable VT loop back to let L2 switch to do it. */
        config.vmdq_conf_default->rx_adv_conf.vmdq_rx_conf.enable_loop_back = 1;
        RTE_LOG(DEBUG, VHOST_CONFIG, "Enable loop back for L2 switch in vmdq.\n");
    }

    /* initialize all ports */
    RTE_ETH_FOREACH_DEV(portid) {
        /* skip ports that are not enabled */
        if ((config.enable_port_mask & (1 << portid)) == 0) {
            RTE_LOG(INFO, VHOST_PORT, "Skipping disabled port %d\n", portid);
            continue;
        }
        if (port_init(portid) != 0)
            rte_exit(EXIT_FAILURE, "Cannot initialize network ports\n");
    }

    // NEW: Initialize TCP offload subsystem
    if (tcp_offload_init(128 * 1024) != 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize TCP offload\n");
    }
    RTE_LOG(INFO, VHOST_CONFIG, "TCP offload initialized (pass-through mode)\n");

    /* Enable stats if the user option is set. */
    if (config.enable_stats) {
        ret = rte_ctrl_thread_create(&tid, "print-stats", NULL, print_stats, NULL);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot create print-stats thread\n");
    }

    /* Launch all data cores. */
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    rte_eal_remote_launch(switch_worker, NULL, lcore_id);

    if (config.client_mode)
        flags |= RTE_VHOST_USER_CLIENT;

    if (config.dequeue_zero_copy)
        flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;

    /* Register vhost user driver to handle vhost messages. */
    for (i = 0; i < config.nb_sockets; i++) {
        char *file = config.socket_files + i * PATH_MAX;
        ret = rte_vhost_driver_register(file, flags);
        if (ret != 0) {
            unregister_drivers(i);
            rte_exit(EXIT_FAILURE, "vhost driver register failure.\n");
        }

        if (config.builtin_net_driver)
            rte_vhost_driver_set_features(file, VIRTIO_NET_FEATURES);

        if (config.mergeable == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_MRG_RXBUF);
        }

        if (config.enable_tx_csum == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_CSUM);
        }

        if (config.enable_tso == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO4);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO6);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO4);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO6);
        }

        if (config.promiscuous) {
            rte_vhost_driver_enable_features(file, 1ULL << VIRTIO_NET_F_CTRL_RX);
        }

        ret = rte_vhost_driver_callback_register(file, &virtio_net_device_ops);
        if (ret != 0) {
            rte_exit(EXIT_FAILURE, "failed to register vhost driver callbacks.\n");
        }

        if (rte_vhost_driver_start(file) < 0) {
            rte_exit(EXIT_FAILURE, "failed to start vhost driver.\n");
        }
    }

    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    rte_eal_wait_lcore(lcore_id);

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}
