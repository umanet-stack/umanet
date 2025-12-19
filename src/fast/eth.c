#include <generic/rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/fast/network.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"

// receive packets from physical NIC and forward them to a VM
void poll_eth_rx(struct vhost_dev *vdev) {
    uint16_t rx_count, enqueue_count;
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    struct dataplane_context *ctx = ctxs[vdev->coreid];

    if (unlikely(check_device_state(vdev, "poll_eth_rx") != 0))
        return;

    rx_count = rte_eth_rx_burst(net_port_id, vdev->rx_queue, pkts, MAX_PKT_BURST);
    if (!rx_count)
        return;
    LOG_ETH_IN("Received %d packets from physical NIC\n", rx_count);
    PRINT_PKTS(pkts, rx_count, LOG_ETH_IN);

// Batching structure: collect packets per destination VM
// Max VID is typically small (<32), use array for O(1) lookup
#define MAX_VID 32
    struct {
        struct rte_mbuf *pkts[MAX_PKT_BURST];
        uint16_t count;
        struct vhost_dev *vdev;
    } batches[MAX_VID];
    memset(batches, 0, sizeof(batches));

    uint16_t local_count = 0; // Packets for this vdev
    struct rte_mbuf *local_pkts[MAX_PKT_BURST];

    // Sort packets by destination VM (batching phase)
    for (uint16_t i = 0; i < rx_count; i++) {
        int target_vid = -1;
        struct vhost_dev *target_vdev = NULL;
        // if (nat_translate_inbound(pkts[i], &target_vid) == 0) {
        // }
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkts[i], struct rte_ether_hdr *);
        target_vdev = find_vhost_dev_core(ctx, &eth_hdr->d_addr);

        if (target_vdev != NULL) {
            target_vid = target_vdev->vid;
            if (target_vid == vdev->vid) {
                // For this VM
                local_pkts[local_count++] = pkts[i];
                continue;
            }
        } else {
            // Unknown destination - deliver to current VM
            local_pkts[local_count++] = pkts[i];
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
        if (batches[vid].count > 0) {
            LOG_ETH_IN("Forwarding %d packets to vid=%d\n", batches[vid].count, vid);
            // vhost enqueue: pkts are COPIED to guest shared memory, must free
            uint16_t sent = rte_vhost_enqueue_burst(vid, VIRTIO_RXQ, batches[vid].pkts, batches[vid].count);
            if (sent < batches[vid].count) {
                LOG_WARN("Failed to forward %d/%d packets to vid=%d\n", batches[vid].count - sent, batches[vid].count,
                         vid);
            }
            // Free ALL packets (enqueue copies them to guest memory)
            free_pkts(batches[vid].pkts, batches[vid].count);
        }
    }

    if (local_count == 0)
        return;

    enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, local_pkts, local_count);
    LOG_ETH_OUT("Enqueued %d packets to guest virtio RX ring\n", enqueue_count);
    PRINT_PKTS(local_pkts, enqueue_count, LOG_ETH_OUT);

    if (unlikely(enqueue_count == 0 && local_count > 0)) {
        LOG_WARN("Warning: Failed to enqueue any packets to vid=%d (may be disconnected)\n", vdev->vid);
        free_pkts(local_pkts, local_count);
        vdev->remove = 1; // Mark device for removal
        return;
    }

    /* Retry if necessary */
    if (config.enable_retry && unlikely(enqueue_count < local_count)) {
        uint32_t retry = 0;

        while (enqueue_count < local_count && retry++ < config.burst_rx_retry_num) { // max 4 retries
            rte_delay_us(config.burst_rx_delay_time);
            enqueue_count +=
                rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &local_pkts[enqueue_count], local_count - enqueue_count);
        }
    }

    if (config.enable_stats) {
        rte_atomic64_add(&vdev->stats.rx_total_atomic, local_count);
        rte_atomic64_add(&vdev->stats.rx_atomic, enqueue_count);
    }

    free_pkts(local_pkts, local_count);
}

// moves packets from a software staging buffer (tx_q->m_table) to the NIC's hardware TX queue/ring
void flush_eth_tx(struct mbuf_table *tx_q) {
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
    struct rte_ether_addr gateway_mac = {{0x18, 0x5a, 0x58, 0x34, 0x49, 0xe4}}; // Physical gateway MAC
    for (int i = 0; i < tx_q->len; i++) {
        eth_hdr = rte_pktmbuf_mtod(tx_q->m_table[i], struct rte_ether_hdr *);
        rte_ether_addr_copy(&eth_addr, &eth_hdr->s_addr);    // Src: NIC's MAC
        rte_ether_addr_copy(&gateway_mac, &eth_hdr->d_addr); // Dst: Gateway's MAC
    }

    // Packets are given to NIC hardware, NIC takes ownership and frees after DMA completes (don't free yourself)
    count = rte_eth_tx_burst(net_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
    LOG_ETH_OUT("(%d) Sent %d packets to NIC\n", tx_q->txq_id, count);
    PRINT_PKTS(tx_q->m_table, count, LOG_ETH_OUT);

    if (unlikely(count < tx_q->len))                         // fewer packets were sent than attempted
        free_pkts(&tx_q->m_table[count], tx_q->len - count); // free the unsent packets

    tx_q->len = 0; // reset the queue length
}