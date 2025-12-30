#include "log.h"
#include "src/include/main.h"
#include "src/slow/slowpath.h"
#include <rte_arp.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>
#include <rte_vhost.h>

int process_arp_req(struct control_ctx *ctx, uint16_t vid, struct rte_mbuf *m, enum slow_src src) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    if (arp->arp_opcode != rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
        LOG_WARN("[slow] ARP: Not a request, ignoring\n");
        return -1; // Not handled, should be forwarded
    }

    uint32_t req_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip); // big to little endian
    if (req_ip != config.ip) {
        // ARP request for another VM, should be broadcast to all VMs
        LOG_INFO("[%d] ARP: Request for VM IP %u.%u.%u.%u, forwarding to other VMs\n", ctx->core_id,
                 (req_ip >> 24) & 0xff, (req_ip >> 16) & 0xff, (req_ip >> 8) & 0xff, req_ip & 0xff);
        return -1; // Not for gateway, forward to VMs
    }

    LOG_INFO("[%d] ARP: Request for IP %u.%u.%u.%u, sending reply to VM %d\n", ctx->core_id,
             (req_ip >> 24) & 0xff, (req_ip >> 16) & 0xff, (req_ip >> 8) & 0xff, req_ip & 0xff, vid);
    rte_ether_addr_copy(&eth->src_addr, &eth->dst_addr); // dst MAC = src MAC
    rte_ether_addr_copy(&config.mac, &eth->src_addr);    // src MAC = our MAC

    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

    // Save original sender MAC/IP (the VM that sent the request)
    uint32_t orig_ip = arp->arp_data.arp_sip;
    struct rte_ether_addr orig_mac;
    rte_ether_addr_copy(&arp->arp_data.arp_sha, &orig_mac);

    // Sender = our MAC/IP (we are the gateway)
    rte_ether_addr_copy(&config.mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(config.ip);

    // Target = original sender MAC/IP (the VM)
    rte_ether_addr_copy(&orig_mac, &arp->arp_data.arp_tha);
    arp->arp_data.arp_tip = orig_ip;

    if (src == SLOW_SRC_VHOST) {
        int ret = rte_ring_enqueue_burst(global->vhost_tx_rings[vid], (void **)&m, 1, NULL);
        if (unlikely(ret == 0))
            LOG_WARN("[%d] Failed to enqueue ARP reply to vid=%d\n", ctx->core_id, vid);
    }
    // else if (src == SLOW_SRC_ETH) { // handled in from_eth instead
    //     // int ret = network_send(ctx, 1, &m);
    //     if (unlikely(ret == 0))
    //         LOG_WARN("[%d] Failed to send ARP reply to physical NIC\n", ctx->core_id);
    // }

    rte_pktmbuf_free(m);

    return 0; // Handled successfully
}