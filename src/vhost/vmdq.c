/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_mbuf_core.h>

#include "log.h"
#include "src/include/state.h"
#include "src/vhost/vhost.h"

extern struct rte_hash *mac_lookup_table;

// learns the MAC address, IPv4 of the device
int link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m) {
    int i, ret;
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    // if (find_vhost_dev(&eth_hdr->src_addr)) {
    //     LOG_WARN("(%d) device is using a registered MAC!\n", vdev->vid);
    //     return -1;
    // }

    for (i = 0; i < RTE_ETHER_ADDR_LEN; i++)
        vdev->mac.addr_bytes[i] = eth_hdr->src_addr.addr_bytes[i];

    LOG_IMPT("(%d) mac %02x:%02x:%02x:%02x:%02x:%02x registered\n", vdev->vid, vdev->mac.addr_bytes[0],
             vdev->mac.addr_bytes[1], vdev->mac.addr_bytes[2], vdev->mac.addr_bytes[3], vdev->mac.addr_bytes[4],
             vdev->mac.addr_bytes[5]);

    ret = rte_eth_dev_mac_addr_add(global->eth_port_id, &vdev->mac, 0);
    if (ret)
        LOG_ERROR("(%d) failed to add device MAC address\n", vdev->vid);

    // register IP address
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        vdev->ip = rte_be_to_cpu_32(ipv4_hdr->src_addr);
        LOG_IMPT("(%d) IP address %u.%u.%u.%u registered from IP packet\n", vdev->vid, (vdev->ip >> 24) & 0xff,
                 (vdev->ip >> 16) & 0xff, (vdev->ip >> 8) & 0xff, vdev->ip & 0xff);
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
        // source IP (arp_sip) = VM's IP
        vdev->ip = rte_be_to_cpu_32(arp_hdr->arp_data.arp_sip);
        LOG_IMPT("(%d) IP address %u.%u.%u.%u registered from ARP packet\n", vdev->vid, (vdev->ip >> 24) & 0xff,
                 (vdev->ip >> 16) & 0xff, (vdev->ip >> 8) & 0xff, vdev->ip & 0xff);
    }

    ret = add_route_entry(vdev->vid, &vdev->mac, vdev->ip);
    if (ret)
        LOG_ERROR("(%d) failed to add route entry\n", vdev->vid);

    vdev->ready = DEVICE_RX;

    return 0;
}

/*
 * Removes MAC address from VMDQ. Ensures that nothing is adding buffers to the RX
 * queue before disabling RX on the device.
 */
void unlink_vmdq(struct vhost_dev *vdev) {
    unsigned i = 0;
    int ret;

    if (vdev->ready == DEVICE_RX) {
        ret = rte_eth_dev_mac_addr_remove(global->eth_port_id, &vdev->mac);
        if (ret)
            LOG_ERROR("(%d) failed to remove MAC address\n", vdev->vid);

        ret = remove_route_entry(&vdev->mac, vdev->ip);
        if (ret)
            LOG_ERROR("(%d) failed to remove route entry\n", vdev->vid);

        for (i = 0; i < 6; i++)
            vdev->mac.addr_bytes[i] = 0;
        vdev->ip = 0;

        /*Clear out the receive buffers*/
        // rx_count = network_poll(ctx, MAX_PKT_BURST, pkts_burst);

        // while (rx_count) { // until queue is empty
        //     free_pkts(pkts_burst, rx_count);
        //     rx_count = network_poll(ctx, MAX_PKT_BURST, pkts_burst);
        // }

        vdev->ready = DEVICE_SAFE_REMOVE;
    }
}
