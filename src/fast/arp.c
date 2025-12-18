#include "src/include/fastpath.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
#include <rte_arp.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>
#include <rte_vhost.h>

void process_arp(struct vhost_dev *vdev, struct rte_mbuf *m) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    if (arp->arp_opcode != rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
        LOG_WARN("(%d) ARP: Not a request (%d != %d)\n", vdev->vid, arp->arp_opcode,
                 rte_cpu_to_be_16(RTE_ARP_OP_REQUEST));
        return; // Not a request
    }

    uint32_t req_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip); // big to little endian
    if (req_ip != config.ip) {
        LOG_WARN("(%d) ARP: Not for us (%u.%u.%u.%u != %u.%u.%u.%u)\n", vdev->vid, (req_ip >> 24) & 0xff,
                 (req_ip >> 16) & 0xff, (req_ip >> 8) & 0xff, req_ip & 0xff, (config.ip >> 24) & 0xff,
                 (config.ip >> 16) & 0xff, (config.ip >> 8) & 0xff, config.ip & 0xff);
        return; // Not for us
    }

    // Swap Ethernet addresses
    rte_ether_addr_copy(&eth->s_addr, &eth->d_addr); // dst MAC = src MAC
    rte_ether_addr_copy(&config.mac, &eth->s_addr);  // src MAC = our MAC

    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

    // Sender = our MAC/IP
    rte_ether_addr_copy(&config.mac, &arp->arp_data.arp_sha);
    memcpy(&arp->arp_data.arp_sip, &config.ip, 4);

    // Target = original sender MAC/IP
    rte_ether_addr_copy(&arp->arp_data.arp_sha, &arp->arp_data.arp_tha);
    memcpy(&arp->arp_data.arp_tip, &arp->arp_data.arp_sip, 4);

    // Send back to VM
    int ret = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, &m, 1);
    if (unlikely(ret == 0))
        LOG_WARN("Warning: Failed to enqueue packet to vid=%d\n", vdev->vid);
    LOG_PKT_OUT("(%d) Sent ARP reply to VM\n", vdev->vid);
    PRINT_PKTS(&m, 1, LOG_PKT_OUT);

    return;
}