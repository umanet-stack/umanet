#include "log.h"
#include "src/include/fastpath.h"
#include "src/include/tas.h"
#include "src/vhost/vhost.h"
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <string.h>

// Global hash table for IP address to flow lookup
struct rte_hash *mac_flow_table = NULL;
#define MAC_FLOW_TABLE_SIZE 256 // Support up to 256 devices

struct rte_flow_rule *flow_rules[MAC_FLOW_TABLE_SIZE];

int init_mac_flow_table(void) {
    struct rte_hash_parameters hash_params = {
        .name = "mac_flow_table",
        .entries = MAC_FLOW_TABLE_SIZE,
        .key_len = sizeof(uint32_t), // IP address as key
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    mac_flow_table = rte_hash_create(&hash_params);
    if (mac_flow_table == NULL) {
        LOG_ERROR("Failed to create mac flow table\n");
        return -1;
    }

    LOG_INFO("MAC flow table initialized (size=%d, key=IP address)\n", MAC_FLOW_TABLE_SIZE);
    return 0;
}

struct rte_ether_addr *install_mac_flow(uint16_t port_id, uint32_t dst_ip, uint16_t queue_id) {
    // Find VM by IP to get its MAC address
    // VM IPs are 192.168.101.x where x starts at 2 (VM 0), 3 (VM 1), etc.
    uint32_t subnet_base = (dst_ip & 0xFFFFFF00); // Get /24 subnet
    uint32_t vm_base = (config.ip & 0xFFFFFF00);  // Gateway IP subnet

    // Only process if IP is in VM subnet
    if (subnet_base != vm_base || (dst_ip & 0xFF) < 2) {
        LOG_ERROR("IP %u.%u.%u.%u is not in VM subnet\n", (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
                  (dst_ip >> 8) & 0xff, dst_ip & 0xff);
        return NULL;
    }

    uint8_t vm_id = (dst_ip & 0xFF) - 2; // Calculate VM ID from IP

    struct rte_ether_addr *mac = NULL;
    extern struct dataplane_context **ctxs;
    extern unsigned fp_cores_max;

    // Search all cores for the VM with matching VID
    for (int core = 0; core < fp_cores_max; core++) {
        if (ctxs[core] == NULL)
            continue;
        for (int j = 0; j < ctxs[core]->vhost.device_num; j++) {
            struct vhost_dev *vdev = ctxs[core]->vhost.vdev_list[j];
            if (vdev != NULL && vdev->ready == DEVICE_RX && vdev->vid == vm_id) {
                mac = &vdev->mac_address;
                break;
            }
        }
        if (mac != NULL)
            break;
    }

    if (mac == NULL) {
        LOG_ERROR("Failed to find VM for IP %u.%u.%u.%u (VM ID %d)\n", (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
                  (dst_ip >> 8) & 0xff, dst_ip & 0xff, vm_id);
        return NULL;
    }

    struct rte_flow_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;

    /* ---------- Pattern ---------- */
    struct rte_flow_item pattern[2];
    memset(pattern, 0, sizeof(pattern));

    struct rte_flow_item_eth eth_spec;
    struct rte_flow_item_eth eth_mask;
    memset(&eth_spec, 0, sizeof(eth_spec));
    memset(&eth_mask, 0, sizeof(eth_mask));

    memcpy(eth_spec.dst.addr_bytes, mac, 6);
    memset(eth_mask.dst.addr_bytes, 0xff, 6);

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    /* ---------- Actions ---------- */
    struct rte_flow_action actions[2];
    memset(actions, 0, sizeof(actions));

    struct rte_flow_action_queue queue = {
        .index = queue_id,
    };

    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;

    /* ---------- Create ---------- */
    struct rte_flow_error error;
    struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, actions, &error);

    if (!flow) {
        LOG_ERROR("Flow create failed (queue %u): %s\n", queue_id, error.message ? error.message : "unknown");
        return NULL;
    }

    LOG_INFO("Flow created for IP %u.%u.%u.%u (VM %d) mac=%02x:%02x:%02x:%02x:%02x:%02x, queue=%u\n",
             (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff, (dst_ip >> 8) & 0xff, dst_ip & 0xff, vm_id,
             mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4],
             mac->addr_bytes[5], queue_id);

    // Store flow using IP as key
    int ret = rte_hash_add_key_data(mac_flow_table, &dst_ip, (void *)flow);
    if (ret != 0) {
        LOG_ERROR("Failed to add flow to mac flow table for IP %u.%u.%u.%u\n", (dst_ip >> 24) & 0xff,
                  (dst_ip >> 16) & 0xff, (dst_ip >> 8) & 0xff, dst_ip & 0xff);
        return NULL;
    }

    LOG_IMPT("Flow added to mac flow table for IP %u.%u.%u.%u (VM %d), queue=%u\n", (dst_ip >> 24) & 0xff,
             (dst_ip >> 16) & 0xff, (dst_ip >> 8) & 0xff, dst_ip & 0xff, vm_id, queue_id);

    return mac;
}

// struct rte_flow_filter filter = {
//     .type = RTE_FLOW_FILTER_TYPE_ETH,
//     .eth =
//         {
//             .dst_addr = eth_hdr->d_addr,
//         },
// };
// struct rte_flow_action action = {
//     .type = RTE_FLOW_ACTION_TYPE_OUTPUT,
//     .output = {.port = target_vdev->port},
// };
// struct rte_flow_rule rule = {
//     .attr = &attr,
//     .filter = &filter,
//     .action = &action,
// };
// struct rte_flow_error error;
// int ret = rte_flow_create(ctx->net.port_id, &rule, &error);
// if (ret != 0) {
//     LOG_WARN("Failed to create flow rule for dest mac addr=%02x:%02x:%02x:%02x:%02x:%02x, error=%s\n",
//              eth_hdr->d_addr.addr_bytes[0], eth_hdr->d_addr.addr_bytes[1], eth_hdr->d_addr.addr_bytes[2],
//              eth_hdr->d_addr.addr_bytes[3], eth_hdr->d_addr.addr_bytes[4], eth_hdr->d_addr.addr_bytes[5],
//              error.message);
//     continue;
// }
// rte_flow_destroy(ctx->net.port_id, &rule, &error);