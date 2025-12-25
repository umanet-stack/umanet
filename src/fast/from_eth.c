#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_hash.h>
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

    // Sort packets by destination VM (batching phase)
    for (uint16_t i = 0; i < rx_count; i++) {
        int target_vid = -1;
        struct vhost_dev *target_vdev = NULL;
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);

        // flow steering by mac address
        // pkts that are not for vm will also get installed to a flow table
        flow = NULL;
        rte_hash_lookup_data(mac_flow_table, &eth_hdr->dst_addr, (void **)&flow);
        if (unlikely(flow == NULL)) {
            // vm MAC: 12:34:56:78:90:xx
            uint8_t vm_id = eth_hdr->dst_addr.addr_bytes[5] - '0';
            install_mac_flow(net_port_id, &eth_hdr->dst_addr, vm_id % fp_cores_max);
        }

        // TODOZ: Use a hash table keyed by MAC address
        target_vdev = find_vhost_dev_core(ctx, &eth_hdr->dst_addr);

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

        LOG_VM_OUT("Forwarding %d packets to vid=%d\n", batches[vid].count, vid);
        PRINT_PKTS(batches[vid].pkts, batches[vid].count, LOG_VM_OUT);
        // vhost enqueue: pkts are COPIED to guest shared memory, must free
        uint16_t sent = vhost_send(ctx, batches[vid].count, vid, batches[vid].pkts);
        if (sent < batches[vid].count) {
            LOG_WARN("Failed to forward %d/%d packets to vid=%d\n", batches[vid].count - sent, batches[vid].count, vid);
        }
        // Free ALL packets (enqueue copies them to guest memory)
        free_pkts(batches[vid].pkts, batches[vid].count);

        /* Retry if necessary */
        if (config.enable_retry && unlikely(sent < batches[vid].count)) {
            uint32_t retry = 0;

            while (sent < batches[vid].count && retry++ < config.burst_rx_retry_num) { // max 4 retries
                rte_delay_us(config.burst_rx_delay_time);
                sent += vhost_send(ctx, batches[vid].count - sent, vid, &batches[vid].pkts[sent]);
            }
        }

        if (config.enable_stats) {
            rte_atomic64_add(&batches[vid].vdev->stats.rx_total_atomic, batches[vid].count);
            rte_atomic64_add(&batches[vid].vdev->stats.rx_atomic, sent);
        }
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
    // MAC of enp23s0f0np0 of other node
    struct rte_ether_addr gateway_mac = {{0x40, 0xa6, 0xb7, 0xc3, 0x51, 0xc8}};

    for (int i = 0; i < tx_q->len; i++) {
        eth_hdr = rte_pktmbuf_mtod(tx_q->m_table[i], struct rte_ether_hdr *);
        rte_ether_addr_copy(&eth_addr, &eth_hdr->src_addr); // Src: NIC's MAC

        // Preserve broadcast/multicast MACs (for ARP requests, etc.)
        if (rte_is_broadcast_ether_addr(&eth_hdr->dst_addr) || rte_is_multicast_ether_addr(&eth_hdr->dst_addr)) {
            // Keep broadcast/multicast - don't change
        } else if (rte_is_same_ether_addr(&eth_hdr->dst_addr, &config.mac)) {
            // VM sent to gateway MAC - forward to other node's MAC
            rte_ether_addr_copy(&gateway_mac, &eth_hdr->dst_addr);
        }
        // Otherwise, keep the original destination MAC (for direct communication)
    }

    // Packets are given to NIC hardware, NIC takes ownership and frees after DMA completes (don't free yourself)
    count = network_send(ctx, tx_q->len, tx_q->m_table);
    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}