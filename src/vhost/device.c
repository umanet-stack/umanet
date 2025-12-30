/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "../include/main.h"
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_vhost.h>
#include <stdatomic.h>
#include <unistd.h>

#include "log.h"
#include "src/include/state.h"
#include "src/vhost/vhost.h"

// Global hash table for MAC address to vhost_dev lookup
struct rte_hash *mac_lookup_table = NULL;
#define MAC_LOOKUP_TABLE_SIZE 256 // Support up to 256 devices

int check_device_state(struct vhost_dev *vdev, const char *func) {
    if (unlikely(vdev == NULL)) {
        LOG_ERROR("Error: NULL vdev in %s\n", func);
        return -1;
    }

    if (unlikely(vdev->vid < 0 || vdev->vid >= 64)) {
        LOG_ERROR("Error: Invalid vid=%d in %s (possible use-after-free)\n", vdev->vid, func);
        return -1;
    }

    if (unlikely(vdev->remove || vdev->ready == DEVICE_SAFE_REMOVE)) {
        LOG_WARN("Warning: Attempting to %s device vid=%d marked for removal (ready=%d, remove=%d)\n", func, vdev->vid,
                 vdev->ready, vdev->remove);
        return -1;
    }

    return 0;
}

// struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac) {
//     if (unlikely(mac_lookup_table == NULL)) {
//         // Hash table not initialized yet, fall back to linear search
//         struct vhost_dev *vdev;
//         for (int i = 0; i < global->fp_cores; i++) {
//             struct dataplane_context *ctx = ctxs[i];
//             if (ctx == NULL)
//                 continue;
//             for (int j = 0; j < ctx->vhost.device_num; j++) {
//                 vdev = ctx->vhost.vdev_list[j];
//                 if (vdev != NULL && vdev->ready == DEVICE_RX && rte_is_same_ether_addr(mac, &vdev->mac_address))
//                     return vdev;
//             }
//         }
//         return NULL;
//     }

//     // Fast O(1) hash lookup
//     struct vhost_dev *vdev = NULL;
//     int ret = rte_hash_lookup_data(mac_lookup_table, mac, (void **)&vdev);
//     if (ret >= 0 && vdev != NULL && vdev->ready == DEVICE_RX) {
//         return vdev;
//     }

//     return NULL;
// }

// struct vhost_dev *find_vhost_dev_core_ip(struct dataplane_context *ctx, uint32_t vm_ip_address) {
//     struct vhost_dev *vdev;
//     for (int j = 0; j < ctx->vhost.device_num; j++) {
//         vdev = ctx->vhost.vdev_list[j];
//         if (vdev != NULL && vdev->ready == DEVICE_RX && vdev->vm_ip_address == vm_ip_address)
//             return vdev;
//     }
//     return NULL;
// }

// Search for VM device by IP address across all cores (similar to find_vhost_dev for MAC)
// struct vhost_dev *find_vhost_dev_ip(uint32_t vm_ip_address) {
//     struct vhost_dev *vdev;
//     for (int i = 0; i < global->fp_cores; i++) {
//         struct dataplane_context *ctx = ctxs[i];
//         if (ctx == NULL)
//             continue;
//         for (int j = 0; j < ctx->vhost.device_num; j++) {
//             vdev = ctx->vhost.vdev_list[j];
//             if (vdev != NULL && vdev->ready == DEVICE_RX && vdev->vm_ip_address == vm_ip_address)
//                 return vdev;
//         }
//     }
//     return NULL;
// }

// struct vhost_dev *find_vhost_dev_core_mac(struct dataplane_context *ctx, struct rte_ether_addr *mac) {
//     struct vhost_dev *vdev;
//     for (int j = 0; j < ctx->vhost.device_num; j++) {
//         vdev = ctx->vhost.vdev_list[j];
//         if (vdev != NULL && vdev->ready == DEVICE_RX && rte_is_same_ether_addr(mac, &vdev->mac_address))
//             return vdev;
//     }
//     return NULL;
// }

/*
 * Remove a device from the specific data core linked list and from the
 * main linked list. Synchonization  occurs through the use of the
 * lcore dev_removal_flag. Device is made volatile here to avoid re-ordering
 * of dev->remove=1 which can cause an infinite loop in the rte_pause loop.
 */
static void destroy_device(int vid) {
    LOG_IMPT("destroy_device called for vid=%d\n", vid);

    struct vdev_list *old, *new;
    for (;;) {
        old = atomic_load_explicit(&vdev_list, memory_order_acquire);

        new = rte_malloc(NULL, sizeof(*new), RTE_CACHE_LINE_SIZE);
        if (!new) {
            LOG_ERROR("allocation failed\n");
            continue;
        }
        memcpy(new, old, sizeof(*new));

        int idx = -1;
        for (int i = 0; i < old->num; i++) {
            if (old->vdevs[i].vid == vid) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            LOG_WARN("Warning: device vid=%d not found during destroy\n", vid);
            rte_free(new);
            return;
        }

        for (int i = idx; i < old->num - 1; i++) {
            new->vdevs[i] = old->vdevs[i + 1];
        }
        new->num = old->num - 1;

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

    LOG_INFO("Found device vid=%d, marking for removal\n", vid);

    /* Set the remove flag with memory barrier to ensure visibility */
    // __sync_synchronize();
    // vdev->remove = 1;
    // __sync_synchronize();

    // /* Wait for dataplane to acknowledge removal (with timeout) */
    // // Give dataplane time to wake up and process removal (dataplane sleeps 100ms)
    // int max_wait_ms = 5000; // 5 seconds max
    // int wait_ms = 0;
    // while (vdev->ready != DEVICE_SAFE_REMOVE && wait_ms < max_wait_ms) {
    //     usleep(10000); // Sleep 10ms between checks
    //     wait_ms += 10;
    // }

    // if (wait_ms >= max_wait_ms) {
    //     LOG_WARN("Warning: Timeout waiting for device vid=%d removal acknowledgment after %dms\n", vid, wait_ms);
    //     // Force removal anyway to prevent resource leak
    // } else {
    //     LOG_INFO("Device vid=%d removal acknowledged after %dms\n", vid, wait_ms);
    // }

    // LOG_INFO("(%d) device has been removed from vdev_list (device_num now=%d)\n", vdev->vid, vdev_list->num);

    // // Remove from MAC lookup table if MAC was registered
    // if (mac_lookup_table != NULL && vdev->ready == DEVICE_RX) {
    //     int ret = rte_hash_del_key(mac_lookup_table, &vdev->mac_address);
    //     if (ret < 0 && ret != -ENOENT) {
    //         LOG_WARN("Warning: Failed to remove MAC from lookup table for vid=%d (ret=%d)\n", vdev->vid, ret);
    //     }
    // }

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
    vdev->ready = DEVICE_MAC_LEARNING;
    vdev->remove = 0;

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

        new->vdevs[new->num].vid = vid;
        new->num++;

        if (atomic_compare_exchange_weak_explicit(&vdev_list, &old, new, memory_order_release, memory_order_acquire)) {
            break; // success
        }

        // CAS failed — someone updated concurrently
        rte_free(new);
    }

    /* Disable notifications. */
    // Normally, guest would send interrupt when it adds packets to TX queue or consumes packets from RX queue
    // In poll mode, we don't need these interrupts (we constantly poll)
    // This is critical for performance, avoids expensive VM exits
    rte_vhost_enable_guest_notification(vid, VIRTIO_RXQ, 0);
    rte_vhost_enable_guest_notification(vid, VIRTIO_TXQ, 0);

    // Check negotiated protocol features after device connection
    // uint64_t proto_features;
    // if (rte_vhost_get_negotiated_protocol_features(vid, &proto_features) == 0) {
    //     LOG_INFO("(%d) Negotiated protocol features: 0x%lx\n", vid, proto_features);
    //     if (proto_features & (1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD)) {
    //         LOG_INFO("(%d) Zero-copy enabled: INFLIGHT_SHMFD protocol feature negotiated\n", vid);
    //     } else {
    //         LOG_WARN("(%d) Zero-copy NOT possible: missing INFLIGHT_SHMFD (protocol features: 0x%lx)\n", vid,
    //                  proto_features);
    //     }
    // }

    int res = vhost_rx_plan_add(vid);
    if (res != 0) {
        LOG_ERROR("Failed to add device vid=%d to vhost_rx_plan\n", vid);
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

    // Cleanup MAC lookup hash table
    if (mac_lookup_table != NULL) {
        rte_hash_free(mac_lookup_table);
        mac_lookup_table = NULL;
        LOG_INFO("MAC lookup hash table destroyed\n");
    }
}

static int init_mac_lookup_table(void) {
    struct rte_hash_parameters hash_params = {
        .name = "mac_lookup_table",
        .entries = MAC_LOOKUP_TABLE_SIZE,
        .key_len = sizeof(struct rte_ether_addr),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };

    mac_lookup_table = rte_hash_create(&hash_params);
    if (mac_lookup_table == NULL) {
        LOG_ERROR("Failed to create MAC lookup hash table\n");
        return -1;
    }

    LOG_INFO("MAC lookup hash table initialized (size=%d)\n", MAC_LOOKUP_TABLE_SIZE);
    return 0;
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

    if (init_mac_lookup_table() != 0) {
        LOG_ERROR("Failed to initialize MAC lookup table\n");
        return -1;
    }

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