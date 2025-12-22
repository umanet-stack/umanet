/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_mbuf_core.h>

#include "log.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"

// External reference to MAC lookup table
extern struct rte_hash *mac_lookup_table;

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
        LOG_WARN("(%d) device is using a registered MAC!\n", vdev->vid);
        return -1;
    }

    // copy MAC from pkt to dev
    for (i = 0; i < RTE_ETHER_ADDR_LEN; i++)
        vdev->mac_address.addr_bytes[i] = pkt_hdr->s_addr.addr_bytes[i];

    LOG_INFO("(%d) mac %02x:%02x:%02x:%02x:%02x:%02x registered\n", vdev->vid, vdev->mac_address.addr_bytes[0],
             vdev->mac_address.addr_bytes[1], vdev->mac_address.addr_bytes[2], vdev->mac_address.addr_bytes[3],
             vdev->mac_address.addr_bytes[4], vdev->mac_address.addr_bytes[5]);

    /* Register the MAC address without pool */
    ret = rte_eth_dev_mac_addr_add(net_port_id, &vdev->mac_address, 0);
    if (ret)
        LOG_ERROR("(%d) failed to add device MAC address\n", vdev->vid);

    /* Set device as ready for RX. */
    // Changes state from DEVICE_MAC_LEARNING to DEVICE_RX
    vdev->ready = DEVICE_RX;

    // Add to MAC lookup hash table for fast O(1) lookup
    if (mac_lookup_table != NULL) {
        ret = rte_hash_add_key_data(mac_lookup_table, &vdev->mac_address, vdev);
        if (ret < 0) {
            LOG_WARN("(%d) Failed to add MAC to lookup table (ret=%d)\n", vdev->vid, ret);
            // Continue anyway - fallback to linear search will work
        } else {
            LOG_INFO("(%d) MAC added to lookup table\n", vdev->vid);
        }
    }

    return 0;
}

/*
 * Removes MAC address and vlan tag from VMDQ. Ensures that nothing is adding buffers to the RX
 * queue before disabling RX on the device.
 */
void unlink_vmdq(struct dataplane_context *ctx, struct vhost_dev *vdev) {
    unsigned i = 0;
    unsigned rx_count;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

    if (vdev->ready == DEVICE_RX) {
        // Remove from MAC lookup hash table before clearing MAC
        if (mac_lookup_table != NULL) {
            int ret = rte_hash_del_key(mac_lookup_table, &vdev->mac_address);
            if (ret < 0 && ret != -ENOENT) {
                LOG_WARN("(%d) Failed to remove MAC from lookup table (ret=%d)\n", vdev->vid, ret);
            }
        }

        /*clear MAC and VLAN settings*/
        rte_eth_dev_mac_addr_remove(net_port_id, &vdev->mac_address);
        for (i = 0; i < 6; i++)
            vdev->mac_address.addr_bytes[i] = 0;

        /*Clear out the receive buffers*/
        rx_count = network_poll(ctx, MAX_PKT_BURST, pkts_burst);

        while (rx_count) { // until queue is empty
            free_pkts(pkts_burst, rx_count);
            rx_count = network_poll(ctx, MAX_PKT_BURST, pkts_burst);
        }

        vdev->ready = DEVICE_MAC_LEARNING;
    }
}
