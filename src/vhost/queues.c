/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <rte_mbuf_core.h>

#include "src/fast/network.h"
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

    printf("(%d) mac %02x:%02x:%02x:%02x:%02x:%02x registered\n", vdev->vid, vdev->mac_address.addr_bytes[0],
           vdev->mac_address.addr_bytes[1], vdev->mac_address.addr_bytes[2], vdev->mac_address.addr_bytes[3],
           vdev->mac_address.addr_bytes[4], vdev->mac_address.addr_bytes[5]);

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

        /*Clear out the receive buffers*/
        rx_count = rte_eth_rx_burst(net_port_id, (uint16_t)vdev->rx_queue, pkts_burst, MAX_PKT_BURST);

        while (rx_count) {                 // until queue is empty
            for (i = 0; i < rx_count; i++) // Frees each packet buffer back to mbuf pool
                rte_pktmbuf_free(pkts_burst[i]);

            // Receives next batch of packets from queue
            rx_count = rte_eth_rx_burst(net_port_id, (uint16_t)vdev->rx_queue, pkts_burst, MAX_PKT_BURST);
        }

        vdev->ready = DEVICE_MAC_LEARNING;
    }
}
