/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "../include/tas.h"
#include <rte_malloc.h>
#include <sys/queue.h>

#include "src/fast/network.h"
#include "src/vhost/vhost.h"

struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac) {
    struct vhost_dev *vdev;

    for (int i = 0; i < fp_cores_max; i++) {
        struct dataplane_context *ctx = ctxs[i];
        for (int j = 0; j < ctx->vhost.device_num; j++) {
            vdev = ctx->vhost.vdev_list[j];
            if (vdev != NULL && vdev->ready == DEVICE_RX && rte_is_same_ether_addr(mac, &vdev->mac_address))
                return vdev;
        }
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
    struct dataplane_context *ctx = NULL;
    int dev_idx = -1;

    // Find the device across all contexts
    for (int i = 0; i < fp_cores_max; i++) {
        for (int j = 0; j < ctxs[i]->vhost.device_num; j++) {
            if (ctxs[i]->vhost.vdev_list[j] != NULL && ctxs[i]->vhost.vdev_list[j]->vid == vid) {
                vdev = ctxs[i]->vhost.vdev_list[j];
                ctx = ctxs[i];
                dev_idx = j;
                break;
            }
        }
        if (vdev != NULL)
            break;
    }
    if (!vdev)
        return;
    /*set the remove flag. */
    vdev->remove = 1;
    while (vdev->ready != DEVICE_SAFE_REMOVE) {
        rte_pause();
    }

    // Remove device from array by shifting remaining elements
    if (ctx != NULL && dev_idx >= 0) {
        for (int j = dev_idx; j < ctx->vhost.device_num - 1; j++) {
            ctx->vhost.vdev_list[j] = ctx->vhost.vdev_list[j + 1];
        }
        ctx->vhost.vdev_list[ctx->vhost.device_num - 1] = NULL;
        ctx->vhost.device_num--;
    }

    // tells worker cores to acknowledge they've seen the removal at their next safe point
    /* Set the dev_removal_flag on each lcore. */
    RTE_LCORE_FOREACH_SLAVE(lcore)
    ctx->vhost.dev_removal_flag = REQUEST_DEV_REMOVAL;

    /*
     * Once each core has set the dev_removal_flag to ACK_DEV_REMOVAL
     * we can be sure that they can no longer access the device removed
     * from the linked lists and that the devices are no longer in use.
     */
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        // busy-wait until it acknowledges removal
        while (ctx->vhost.dev_removal_flag != ACK_DEV_REMOVAL)
            rte_pause();
    }

    ctx->vhost.device_num--;

    printf("(%d) device has been removed from data core\n", vdev->vid);

    rte_free(vdev);
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 */
// dpdk automatically assigns vid (0, 1, 2, ...) to each device
static int new_device(int vid) {
    uint32_t device_num_min = 64;
    struct vhost_dev *vdev;
    struct dataplane_context *ctx = NULL;

    // RTE_CACHE_LINE_SIZE: Align to cache line (64 bytes typically) to avoid false sharing between cores
    vdev = rte_zmalloc("vhost device", sizeof(*vdev), RTE_CACHE_LINE_SIZE);
    if (vdev == NULL) {
        printf("(%d) couldn't allocate memory for vhost dev\n", vid);
        return -1;
    }
    vdev->vid = vid;

    // Each device gets 1 RX queue
    vdev->vmdq_rx_q = vid;

    /*reset ready flag*/
    vdev->ready = DEVICE_MAC_LEARNING;
    vdev->remove = 0;

    /* Find a suitable context (lcore) to add the device. */
    for (int i = 0; i < fp_cores_max; i++) {
        if (ctxs[i]->vhost.device_num < device_num_min) {
            device_num_min = ctxs[i]->vhost.device_num;
            ctx = ctxs[i];
        }
    }

    if (ctx == NULL) {
        printf("(%d) couldn't find suitable context\n", vid);
        rte_free(vdev);
        return -1;
    }

    vdev->coreid = ctx->id;

    // Add device to array
    if (ctx->vhost.device_num >= MAX_VHOST_DEVICES_PER_CORE) {
        printf("(%d) too many devices on core %d (max %d)\n", vid, ctx->id, MAX_VHOST_DEVICES_PER_CORE);
        rte_free(vdev);
        return -1;
    }
    ctx->vhost.vdev_list[ctx->vhost.device_num] = vdev;
    ctx->vhost.device_num++;

    /* Disable notifications. */
    // Normally, guest would send interrupt when it adds packets to TX queue or consumes packets from RX queue
    // In poll mode, we don't need these interrupts (we constantly poll)
    // This is critical for performance, avoids expensive VM exits
    rte_vhost_enable_guest_notification(vid, VIRTIO_RXQ, 0);
    rte_vhost_enable_guest_notification(vid, VIRTIO_TXQ, 0);

    printf("(%d) device has been added to data core %d\n", vid, vdev->coreid);

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

void unregister_vhost_drivers(int socket_num, const char *path) {
    int i, ret;

    for (i = 0; i < socket_num; i++) {
        // each path is PATH_MAX bytes apart
        ret = rte_vhost_driver_unregister(path + i * PATH_MAX);
        if (ret != 0)
            printf("Fail to unregister vhost driver for %s.\n", path + i * PATH_MAX);
    }
}

int register_vhost_drivers() {
    uint64_t flags = 0;

    // Note: vdev_list is already initialized in dataplane_context_init()
    // No need to initialize here

    if (config.client_mode)
        flags |= RTE_VHOST_USER_CLIENT;

    if (config.dequeue_zero_copy)
        flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;

    /* Register vhost user driver to handle vhost messages. */
    for (int i = 0; i < config.nb_sockets; i++) {
        char *file = config.socket_files + i * PATH_MAX;
        printf("Registering vhost driver for %s...\n", file);
        if (rte_vhost_driver_register(file, flags) != 0) {
            unregister_vhost_drivers(i, config.socket_files);
            return -1;
        }

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

        if (rte_vhost_driver_callback_register(file, &virtio_net_device_ops) != 0) {
            printf("failed to register vhost driver callbacks.\n");
            return -1;
        }

        if (rte_vhost_driver_start(file) < 0) {
            printf("failed to start vhost driver.\n");
            return -1;
        }
        // testing only
        rte_eth_promiscuous_enable(net_port_id);
        printf("Promiscuous mode enabled for port %d\n", net_port_id);
    }

    printf("Vhost drivers started, waiting for connections...\n");
    return 0;
}