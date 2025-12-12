/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "../include/tas.h"
#include <rte_malloc.h>
#include <sys/queue.h>

#include "src/vhost/vhost.h"

vhost_state_t vhost = {
    .vhost_dev_list = TAILQ_HEAD_INITIALIZER(vhost.vhost_dev_list),
};

struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac) {
    struct vhost_dev *vdev;

    TAILQ_FOREACH(vdev, &vhost.vhost_dev_list, global_vdev_entry) {
        if (vdev->ready == DEVICE_RX && rte_is_same_ether_addr(mac, &vdev->mac_address))
            return vdev;
    }

    return NULL;
}

/*
 * Remove a device from the specific data core linked list and from the
 * main linked list. Synchonization  occurs through the use of the
 * lcore dev_removal_flag. Device is made volatile here to avoid re-ordering
 * of dev->remove=1 which can cause an infinite loop in the rte_pause loop.
 */
static void destroy_device(int vid) {
    struct vhost_dev *vdev = NULL;
    int lcore;

    TAILQ_FOREACH(vdev, &vhost.vhost_dev_list, global_vdev_entry) {
        if (vdev->vid == vid)
            break;
    }
    if (!vdev)
        return;
    /*set the remove flag. */
    vdev->remove = 1;
    while (vdev->ready != DEVICE_SAFE_REMOVE) {
        rte_pause();
    }

    // Remove device from its assigned lcore's device list
    TAILQ_REMOVE(&vhost.lcore_info[vdev->coreid].vdev_list, vdev, lcore_vdev_entry);
    // Remove device from global device list
    TAILQ_REMOVE(&vhost.vhost_dev_list, vdev, global_vdev_entry);

    // tells worker cores to acknowledge they've seen the removal at their next safe point
    /* Set the dev_removal_flag on each lcore. */
    RTE_LCORE_FOREACH_SLAVE(lcore)
    vhost.lcore_info[lcore].dev_removal_flag = REQUEST_DEV_REMOVAL;

    /*
     * Once each core has set the dev_removal_flag to ACK_DEV_REMOVAL
     * we can be sure that they can no longer access the device removed
     * from the linked lists and that the devices are no longer in use.
     */
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        // busy-wait until it acknowledges removal
        while (vhost.lcore_info[lcore].dev_removal_flag != ACK_DEV_REMOVAL)
            rte_pause();
    }

    vhost.lcore_info[vdev->coreid].device_num--;

    RTE_LOG(INFO, VHOST_DATA, "(%d) device has been removed from data core\n", vdev->vid);

    rte_free(vdev);
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 */
static int new_device(int vid) {
    int lcore, core_add = 0;
    uint32_t device_num_min = 64;
    struct vhost_dev *vdev;

    // RTE_CACHE_LINE_SIZE: Align to cache line (64 bytes typically) to avoid false sharing between cores
    vdev = rte_zmalloc("vhost device", sizeof(*vdev), RTE_CACHE_LINE_SIZE);
    if (vdev == NULL) {
        RTE_LOG(INFO, VHOST_DATA, "(%d) couldn't allocate memory for vhost dev\n", vid);
        return -1;
    }
    vdev->vid = vid;

    TAILQ_INSERT_TAIL(&vhost.vhost_dev_list, vdev, global_vdev_entry);
    // Each device gets 1 RX queue
    vdev->vmdq_rx_q = vid;

    /*reset ready flag*/
    vdev->ready = DEVICE_MAC_LEARNING;
    vdev->remove = 0;

    /* Find a suitable lcore to add the device. */
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        if (vhost.lcore_info[lcore].device_num < device_num_min) {
            device_num_min = vhost.lcore_info[lcore].device_num;
            core_add = lcore;
        }
    }
    vdev->coreid = core_add;

    TAILQ_INSERT_TAIL(&vhost.lcore_info[vdev->coreid].vdev_list, vdev, lcore_vdev_entry);
    vhost.lcore_info[vdev->coreid].device_num++;

    /* Disable notifications. */
    // Normally, guest would send interrupt when it adds packets to TX queue or consumes packets from RX queue
    // In poll mode, we don't need these interrupts (we constantly poll)
    // This is critical for performance, avoids expensive VM exits
    rte_vhost_enable_guest_notification(vid, VIRTIO_RXQ, 0);
    rte_vhost_enable_guest_notification(vid, VIRTIO_TXQ, 0);

    RTE_LOG(INFO, VHOST_DATA, "(%d) device has been added to data core %d\n", vid, vdev->coreid);

    return 0;
}

/*
 * These callback allow devices to be added to the data core when configuration
 * has been fully complete.
 */
const struct vhost_device_ops virtio_net_device_ops = {
    .new_device = new_device,
    .destroy_device = destroy_device,
};
