#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_ip.h>
#include <rte_mbuf_core.h>

#include "log.h"
#include "src/fast/internal.h"
#include "src/include/fastpath.h"
#include "src/include/tas.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"

// receive packets from physical NIC and forward them to a VM
void fastpath_from_eth(struct dataplane_context *ctx) {
    uint16_t rx_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    struct rte_flow *flow;

    // STATS_TS(eth_poll_start);
    rx_count = network_poll(ctx, MAX_PKT_BURST, pkts);
    // STATS_TS(eth_poll_end);
    // STATS_TSADD(ctx, cyc_eth_poll, eth_poll_end - eth_poll_start);
    if (rx_count == 0)
        return;

// Batching structure: collect packets per destination VM
// Max VID is typically small (<32), use array for O(1) lookup
#define MAX_VID 32
    struct {
        struct rte_mbuf *pkts[MAX_PKT_BURST];
        uint16_t count;
        struct vhost_dev *vdev;
    } batches[MAX_VID];
    memset(batches, 0, sizeof(batches));

    // Prefetch first few packets (like l2fwd example)
    for (uint16_t j = 0; j < rx_count && j < 4; j++) {
        rte_prefetch0(rte_pktmbuf_mtod(pkts[j], void *));
    }

    // Sort packets by destination VM (batching phase)
    for (uint16_t i = 0; i < rx_count; i++) {
        // Prefetch next packet's data (4 packets ahead, like l2fwd)
        if (likely(i + 4 < rx_count))
            rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 4], void *));

        int target_vid = -1;
        struct vhost_dev *target_vdev = NULL;
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);

        // flow steering by IP address (for packets destined to DPDK NIC MAC)
        flow = NULL;
        if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
            uint32_t dst_ip = rte_be_to_cpu_32(ipv4_hdr->dst_addr);

            // Check if IP is in VM subnet
            uint32_t subnet_base = (dst_ip & 0xFFFFFF00); // Get /24 subnet
            uint32_t vm_base = (config.ip & 0xFFFFFF00);  // Gateway IP subnet

            if (subnet_base == vm_base && (dst_ip & 0xFF) >= 2) {
                // Lookup flow by IP
                rte_hash_lookup_data(mac_flow_table, &dst_ip, (void **)&flow);
                if (unlikely(flow == NULL)) {
                    // Calculate VM ID from IP (VM 0 = .2, VM 1 = .3, etc.)
                    uint8_t vm_id = (dst_ip & 0xFF) - 2;
                    struct rte_ether_addr *target_vm_mac = install_mac_flow(net_port_id, dst_ip, vm_id % fp_cores_max);
                    if (target_vm_mac != NULL) {
                        rte_ether_addr_copy(target_vm_mac, &eth_hdr->dst_addr);
                    }
                }
            }
            target_vdev = find_vhost_dev_core_ip(ctx, dst_ip);
        } else {
            // TODOZ: Use a hash table keyed by MAC address
            target_vdev = find_vhost_dev_core_mac(ctx, &eth_hdr->dst_addr);
        }

        if (target_vdev != NULL) {
            target_vid = target_vdev->vid;
        } else if (process_arp(ctx, NULL, pkts[i], ARP_SRC_ETH) == 0) {
            continue;
        }

        // Add to batch for target VM
        if (target_vdev != NULL && target_vid >= 0 && target_vid < MAX_VID) {
            batches[target_vid].pkts[batches[target_vid].count++] = pkts[i];
            batches[target_vid].vdev = target_vdev;
        } else {
            LOG_WARN("Invalid target vid=%d or vdev not found, dropping packet\n", target_vid);
            rte_pktmbuf_free(pkts[i]);
        }
    }

    // Enqueue batches to other VMs (forwarding phase)
    for (int vid = 0; vid < MAX_VID; vid++) {
        if (batches[vid].count == 0)
            continue;

        for (int i = 0; i < batches[vid].count; i++) {
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(batches[vid].pkts[i], struct rte_ether_hdr *);
            struct vhost_dev *vdev = batches[vid].vdev;
            rte_ether_addr_copy(&vdev->mac_address, &eth_hdr->dst_addr); // dst MAC = vm MAC
            rte_ether_addr_copy(&config.mac, &eth_hdr->src_addr);        // src MAC = our MAC
        }

        // Mark device as active BEFORE sending so it gets polled frequently to receive replies
        // This ensures the device stays active even if some packets fail to enqueue
        LOG_INFO("[%d] Marking device %d active from incoming packet\n", ctx->id, batches[vid].vdev->vid);
        batches[vid].vdev->is_active = 1;
        batches[vid].vdev->empty_poll_count = 0;

        // vhost enqueue: pkts are COPIED to guest shared memory, must free
        uint16_t sent = vhost_send(ctx, batches[vid].count, vid, batches[vid].pkts);
        if (sent < batches[vid].count) {
            LOG_WARN("Failed to forward %d/%d packets to vid=%d\n", batches[vid].count - sent, batches[vid].count, vid);
            // Free packets that failed to enqueue (vhost_send copies successfully enqueued packets)
            free_pkts(&batches[vid].pkts[sent], batches[vid].count - sent);
        }
        // Free successfully enqueued packets (enqueue copies them to guest memory)
        free_pkts(batches[vid].pkts, sent);
    }
}

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
void flush_eth_tx(struct dataplane_context *ctx, struct mbuf_table *tx_q) {
    uint16_t count;
    if (unlikely(tx_q == NULL)) {
        LOG_ERROR("Error: NULL tx_q in flush_eth_tx\n");
        return;
    }

    if (unlikely(tx_q->len == 0))
        return;

    // Gateway mode: Change MACs for proper routing
    // VMs send to gateway MAC 02:00:00:00:00:fe, we forward to physical gateway
    struct rte_ether_hdr *eth_hdr;

    for (int i = 0; i < tx_q->len; i++) {
        eth_hdr = rte_pktmbuf_mtod(tx_q->m_table[i], struct rte_ether_hdr *);
        rte_ether_addr_copy(&eth_addr, &eth_hdr->src_addr); // Src: NIC's MAC

        // Preserve broadcast/multicast MACs (for ARP requests, etc.)
        if (rte_is_broadcast_ether_addr(&eth_hdr->dst_addr) || rte_is_multicast_ether_addr(&eth_hdr->dst_addr)) {
            // Keep broadcast/multicast - don't change
        } else if (rte_is_same_ether_addr(&eth_hdr->dst_addr, &config.mac)) {
            // VM sent to other node NIC's MAC
            rte_ether_addr_copy(&config.other_node_mac, &eth_hdr->dst_addr);
        }
        // Otherwise, keep the original destination MAC (for direct communication)
    }

    // Packets are given to NIC hardware, NIC takes ownership and frees after DMA completes (don't free yourself)
    count = network_send(ctx, tx_q->len, tx_q->m_table);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}