/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "../include/main.h"
#include <linux/virtio_net.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_vhost.h>
#include <stdatomic.h>
#include <unistd.h>

#include "log.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/vhost/vhost.h"

int check_device_state(struct vhost_dev *vdev, const char *func) {
    if (unlikely(vdev == NULL)) {
        LOG_ERROR("Error: NULL vdev in %s\n", func);
        return -1;
    }

    if (unlikely(vdev->vid < 0 || vdev->vid >= 64)) {
        LOG_ERROR("Error: Invalid vid=%d in %s (possible use-after-free)\n", vdev->vid, func);
        return -1;
    }

    if (unlikely(vdev->ready == DEVICE_SAFE_REMOVE)) {
        LOG_WARN("(%d) Attempting to %s device marked for removal (ready=%d)\n", vdev->vid, func, vdev->ready);
        return -1;
    }

    return 0;
}

/*
 * Remove a device from the specific data core linked list and from the
 * main linked list. Synchonization  occurs through the use of the
 * lcore dev_removal_flag. Device is made volatile here to avoid re-ordering
 * of dev->remove=1 which can cause an infinite loop in the rte_pause loop.
 */
static void destroy_device(int vid) {
    LOG_IMPT("destroy_device called for vid=%d\n", vid);
    struct vhost_dev *vdev = vdev_list->vdevs[vid];
    if (vdev == NULL) {
        LOG_ERROR("(%d) device not found during destroy\n", vid);
        return;
    }
    unlink_vmdq(vdev);

    struct vdev_list *old, *new;
    while (1) {
        old = atomic_load_explicit(&vdev_list, memory_order_acquire);
        new = rte_malloc(NULL, sizeof(*new), RTE_CACHE_LINE_SIZE);
        if (!new) {
            LOG_ERROR("allocation failed\n");
            return;
        }
        memcpy(new, old, sizeof(*new));
        if (new->vdevs[vid] == NULL) {
            LOG_WARN("(%d) device  not found during destroy\n", vid);
            rte_free(new);
            return;
        }
        if (new->vdevs[vid]->ready != DEVICE_SAFE_REMOVE) {
            LOG_WARN("(%d) device not in safe remove state during destroy\n", vid);
            rte_free(new);
            return;
        }

        new->num = old->num - 1;
        new->vdevs[vid] = NULL;

        if (atomic_compare_exchange_weak_explicit(&vdev_list, &old, new, memory_order_release, memory_order_acquire)) {
            break; // success
        }

        // CAS failed — someone updated concurrently
        rte_free(new);
    }

    int res = vhost_rx_plan_remove(vid);
    if (res != 0) {
        LOG_ERROR("Failed to remove device vid=%d from vhost_rx_plan\n", vid);
        return;
    }
    res = vhost_tx_plan_remove(vid);
    if (res != 0) {
        LOG_ERROR("Failed to remove device vid=%d from vhost_tx_plan\n", vid);
        return;
    }

    res = uninstall_eth_rx_flow(vdev);
    if (res != 0) {
        LOG_ERROR("Failed to uninstall eth_rx_flow for vid=%d\n", vid);
        return;
    }

    LOG_INFO("Found device vid=%d, marking for removal\n", vid);

    // rte_free(vdev);
}

// dpdk automatically assigns vid (0, 1, 2, ...) to each device
static int new_device(int vid) {
    struct vhost_dev *vdev;

    // RTE_CACHE_LINE_SIZE: Align to cache line (64 bytes typically) to avoid false sharing between cores
    vdev = rte_zmalloc("vhost device", sizeof(*vdev), RTE_CACHE_LINE_SIZE);
    if (vdev == NULL) {
        LOG_ERROR("(%d) couldn't allocate memory for vhost dev\n", vid);
        return -1;
    }
    vdev->vid = vid;
    vdev->vm_id = -1;
    vdev->ready = DEVICE_MAC_LEARNING;
    vdev->mac = (struct rte_ether_addr){0};
    vdev->ip = 0;

    if (vdev_list->num >= MAX_VHOSTS) {
        LOG_ERROR("(%d) too many devices on vdev_list (max %d)\n", vid, MAX_VHOSTS);
        rte_free(vdev);
        return -1;
    }

    struct vdev_list *old, *new;
    while (1) {
        old = atomic_load_explicit(&vdev_list, memory_order_acquire);

        if (old->num >= MAX_VHOSTS) {
            LOG_ERROR("vdev_list full\n");
            return -1;
        }

        new = rte_malloc(NULL, sizeof(*new), RTE_CACHE_LINE_SIZE);
        if (!new) {
            LOG_ERROR("allocation failed\n");
            return -1;
        }

        memcpy(new, old, sizeof(*new));

        new->vdevs[vid] = vdev;
        new->num = old->num + 1;

        if (atomic_compare_exchange_weak_explicit(&vdev_list, &old, new, memory_order_release, memory_order_acquire)) {
            break; // success
        }

        // CAS failed — someone updated concurrently
        rte_free(new);
    }
    LOG_INFO("(%d) device added to vdev_list (total vdev now=%d)\n", vid, vdev_list->num);

    /* Disable notifications. */
    // Normally, guest would send interrupt when it adds packets to TX queue or consumes packets from RX queue
    // In poll mode, we don't need these interrupts (we constantly poll)
    // This is critical for performance, avoids expensive VM exits
    rte_vhost_enable_guest_notification(vid, VIRTIO_RXQ, 0);
    rte_vhost_enable_guest_notification(vid, VIRTIO_TXQ, 0);

    // Check negotiated protocol features after device connection
    uint64_t proto_features;
    if (rte_vhost_get_negotiated_protocol_features(vid, &proto_features) == 0) {
        LOG_IMPT("(%d) Negotiated protocol features: 0x%lx\n", vid, proto_features);
        for (size_t i = 0; i < 34; i++) {
            if (proto_features & features[i].bit) {
                LOG_IMPT("(%d) %s protocol feature negotiated\n", vid, features[i].name);
            }
        }
    }

    int res = vhost_rx_plan_add(vid);
    if (res != 0) {
        LOG_ERROR("Failed to add device vid=%d to vhost_rx_plan\n", vid);
        rte_free(vdev);
        return -1;
    }
    res = vhost_tx_plan_add(vid);
    if (res != 0) {
        LOG_ERROR("Failed to add device vid=%d to vhost_tx_plan\n", vid);
        rte_free(vdev);
        return -1;
    }

    // Note: MAC address will be added to lookup table when learned in link_vmdq()
    return 0;
}

/*
 * These callback allow devices to be added to the data core when configuration
 * has been fully complete.
 */
const struct rte_vhost_device_ops virtio_net_device_ops = {
    .new_device = new_device,
    .destroy_device = destroy_device,
};

void unregister_vhost_drivers(int socket_num, const char *path) {
    int i, ret;

    for (i = 0; i < socket_num; i++) {
        // each path is PATH_MAX bytes apart
        ret = rte_vhost_driver_unregister(path + i * PATH_MAX);
        if (ret != 0)
            LOG_ERROR("Fail to unregister vhost driver for %s.\n", path + i * PATH_MAX);
    }

    // cleanup_route_table();
}

int register_vhost_drivers() {
    uint64_t flags = 0;

    vdev_list = rte_zmalloc("vdev_list", sizeof(*vdev_list), RTE_CACHE_LINE_SIZE);
    if (vdev_list == NULL) {
        LOG_ERROR("Failed to allocate memory for vdev_list\n");
        return -1;
    }
    vdev_list->num = 0;
    memset(vdev_list->vdevs, 0, sizeof(vdev_list->vdevs));

    init_route_table();

    if (config.client_mode)
        flags |= RTE_VHOST_USER_CLIENT;

    // // Zero copy support flags
    // // if (config.dequeue_zero_copy) {
    // // External buffer support enables zero copy (mbufs with external buffers)
    // flags |= RTE_VHOST_USER_EXTBUF_SUPPORT;
    // // Linear buffer support (required for external buffers)
    // flags |= RTE_VHOST_USER_LINEARBUF_SUPPORT;
    // LOG_INFO("Zero copy enabled: EXTBUF_SUPPORT and LINEARBUF_SUPPORT flags set\n");
    // // }

    config.socket_files = malloc(PATH_MAX * config.nb_sockets);
    if (config.socket_files == NULL) {
        LOG_ERROR("failed to allocate memory for socket files.\n");
        return -1;
    }
    for (int i = 0; i < config.nb_sockets; i++) {
        snprintf(config.socket_files + i * PATH_MAX, PATH_MAX, "%s/sock%d", config.socket_dir, i);
    }

    /* Register vhost user driver to handle vhost messages. */
    int registered_count = 0;
    for (int i = 0; i < config.nb_sockets; i++) {
        char *file = config.socket_files + i * PATH_MAX;
        LOG_INFO("Registering vhost driver for %s...\n", file);
        if (rte_vhost_driver_register(file, flags) != 0) {
            LOG_ERROR("Failed to register vhost driver for %s (socket %d/%d)\n", file, i, config.nb_sockets);
            // Continue with other sockets instead of failing completely
            continue;
        }

        // flags describe what the backend (you) and the guest agree on
        uint64_t features = (1ULL << VIRTIO_NET_F_MTU) | (1ULL << VIRTIO_NET_F_MRG_RXBUF) |
                            (1ULL << VIRTIO_NET_F_CTRL_VQ) | (1ULL << VIRTIO_NET_F_CSUM) |
                            (1ULL << VIRTIO_NET_F_GUEST_CSUM) | (1ULL << VIRTIO_NET_F_GUEST_UFO) |
                            (1ULL << VIRTIO_NET_F_HOST_TSO4) | (1ULL << VIRTIO_NET_F_HOST_TSO6) |
                            (1ULL << VIRTIO_NET_F_GUEST_TSO4) | (1ULL << VIRTIO_NET_F_GUEST_TSO6);

        rte_vhost_driver_enable_features(file, features);
        // if (config.mergeable == 0) {
        // }
        // Allows the host to place one large packet across multiple guest RX buffers
        // 1 packet → RX buf 0 + RX buf 1 + RX buf 2
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_MRG_RXBUF);

        // if (config.enable_tx_csum == 0) {
        // }
        // guests won’t compute checksums, host will do it
        // guest sends packets with checksum fields = 0, host fills them later
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_CSUM);
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_CSUM);
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_UFO);
        // //
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_ECN);

        // if (config.enable_tso == 0) {
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO4);
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO6);
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO4);
        // rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO6);
        // // }

        // - RTE_VHOST_USER_EXTBUF_SUPPORT (enables external buffer mbufs)
        // - RTE_VHOST_USER_LINEARBUF_SUPPORT (required for external buffers)
        // - VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD (required for zero copy tracking)
        // uint64_t protocol_features = 0;
        // if (rte_vhost_driver_get_protocol_features(file, &protocol_features) == 0) {
        //     protocol_features |= (1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD);
        //     if (rte_vhost_driver_set_protocol_features(file, protocol_features) != 0) {
        //         LOG_WARN("Failed to set INFLIGHT_SHMFD protocol feature for %s (zero copy may not work)\n", file);
        //     } else {
        //         LOG_INFO("Enabled INFLIGHT_SHMFD protocol feature for zero copy support (features: 0x%lx)\n",
        //                  protocol_features);
        //     }
        // } else {
        //     LOG_WARN("Failed to get protocol features for %s, trying to set INFLIGHT_SHMFD directly\n", file);
        //     protocol_features = (1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD);
        //     if (rte_vhost_driver_set_protocol_features(file, protocol_features) != 0) {
        //         LOG_WARN("Failed to set INFLIGHT_SHMFD protocol feature for %s (zero copy may not work)\n", file);
        //     }
        // }

        if (rte_vhost_driver_callback_register(file, &virtio_net_device_ops) != 0) {
            LOG_ERROR("Failed to register vhost driver callbacks for %s (socket %d/%d)\n", file, i, config.nb_sockets);
            rte_vhost_driver_unregister(file);
            continue;
        }

        if (rte_vhost_driver_start(file) < 0) {
            LOG_ERROR("Failed to start vhost driver for %s (socket %d/%d)\n", file, i, config.nb_sockets);
            rte_vhost_driver_unregister(file);
            continue;
        }

        registered_count++;
        LOG_INFO("Successfully registered and started vhost driver for %s (%d/%d)\n", file, registered_count,
                 config.nb_sockets);
        // Note: Protocol features are negotiated when a device connects, not at driver start
        // Check is done in new_device() callback when each device connects
    }

    if (registered_count == 0) {
        LOG_ERROR("ERROR: Failed to register any vhost drivers!\n");
        return -1;
    }

    if (registered_count < config.nb_sockets) {
        LOG_WARN("WARNING: Only registered %d out of %d vhost drivers\n", registered_count, config.nb_sockets);
    }

    LOG_INFO("Vhost drivers started, waiting for connections...\n");

    return 0;
}

struct vhost_feature features[] = {
    {1ULL << VIRTIO_NET_F_CSUM, "CSUM (host checksum offload)"},
    {1ULL << VIRTIO_NET_F_GUEST_CSUM, "GUEST_CSUM (guest checksum offload)"},
    {1ULL << VIRTIO_NET_F_CTRL_GUEST_OFFLOADS, "CTRL_GUEST_OFFLOADS"},
    {1ULL << VIRTIO_NET_F_MTU, "MTU"},
    {1ULL << VIRTIO_NET_F_MAC, "MAC"},
    {1ULL << VIRTIO_NET_F_GSO, "GSO (legacy)"},
    {1ULL << VIRTIO_NET_F_GUEST_TSO4, "TSO4 (guest)"},
    {1ULL << VIRTIO_NET_F_GUEST_TSO6, "TSO6 (guest)"},
    {1ULL << VIRTIO_NET_F_GUEST_ECN, "ECN (guest)"},
    {1ULL << VIRTIO_NET_F_GUEST_UFO, "UFO (guest)"},
    {1ULL << VIRTIO_NET_F_HOST_TSO4, "TSO4 (host)"},
    {1ULL << VIRTIO_NET_F_HOST_TSO6, "TSO6 (host)"},
    {1ULL << VIRTIO_NET_F_HOST_ECN, "ECN (host)"},
    {1ULL << VIRTIO_NET_F_HOST_UFO, "UFO (host)"},
    {1ULL << VIRTIO_NET_F_MRG_RXBUF, "MRG_RXBUF"},
    {1ULL << VIRTIO_NET_F_STATUS, "STATUS"},
    {1ULL << VIRTIO_NET_F_CTRL_VQ, "CTRL_VQ"},
    {1ULL << VIRTIO_NET_F_CTRL_RX, "CTRL_RX"},
    {1ULL << VIRTIO_NET_F_CTRL_VLAN, "CTRL_VLAN"},
    {1ULL << VIRTIO_NET_F_CTRL_RX_EXTRA, "CTRL_RX_EXTRA"},
    {1ULL << VIRTIO_NET_F_GUEST_ANNOUNCE, "GUEST_ANNOUNCE"},
    {1ULL << VIRTIO_NET_F_MQ, "MQ / RSS"},
    {1ULL << VIRTIO_NET_F_CTRL_MAC_ADDR, "CTRL_MAC_ADDR"},
    {1ULL << VIRTIO_NET_F_VQ_NOTF_COAL, "VQ_NOTF_COAL"},
    {1ULL << VIRTIO_NET_F_NOTF_COAL, "NOTF_COAL"},
    {1ULL << VIRTIO_NET_F_GUEST_USO4, "USO4 (guest)"},
    {1ULL << VIRTIO_NET_F_GUEST_USO6, "USO6 (guest)"},
    {1ULL << VIRTIO_NET_F_HOST_USO, "USO (host)"},
    {1ULL << VIRTIO_NET_F_HASH_REPORT, "HASH_REPORT"},
    {1ULL << VIRTIO_NET_F_GUEST_HDRLEN, "GUEST_HDRLEN"},
    {1ULL << VIRTIO_NET_F_RSS, "RSS"},
    {1ULL << VIRTIO_NET_F_RSC_EXT, "RSC_EXT"},
    {1ULL << VIRTIO_NET_F_STANDBY, "STANDBY"},
    {1ULL << VIRTIO_NET_F_SPEED_DUPLEX, "SPEED_DUPLEX"},
};