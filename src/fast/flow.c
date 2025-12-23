#include "log.h"
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <string.h>

// Global hash table for MAC address to vhost_dev lookup
struct rte_hash *mac_flow_table = NULL;
#define MAC_FLOW_TABLE_SIZE 256 // Support up to 256 devices

struct rte_flow_rule *flow_rules[MAC_FLOW_TABLE_SIZE];

int init_mac_flow_table(void) {
    struct rte_hash_parameters hash_params = {
        .name = "mac_flow_table",
        .entries = MAC_FLOW_TABLE_SIZE,
        .key_len = sizeof(struct rte_ether_addr),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };
    mac_flow_table = rte_hash_create(&hash_params);
    if (mac_flow_table == NULL) {
        LOG_ERROR("Failed to create mac flow table\n");
        return -1;
    }

    LOG_INFO("MAC flow table initialized (size=%d)\n", MAC_FLOW_TABLE_SIZE);
    return 0;
}

struct rte_flow *install_mac_flow(uint16_t port_id, const struct rte_ether_addr *mac, uint16_t queue_id) {
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

    LOG_INFO("Flow created for mac=%02x:%02x:%02x:%02x:%02x:%02x, queue=%u\n", mac->addr_bytes[0], mac->addr_bytes[1],
             mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5], queue_id);

    int ret = rte_hash_add_key_data(mac_flow_table, mac, (void *)flow);
    if (ret != 0) {
        LOG_ERROR("Failed to add flow to mac flow table\n");
        return NULL;
    }

    LOG_INFO("Flow added to mac flow table for mac=%02x:%02x:%02x:%02x:%02x:%02x, queue=%u\n", mac->addr_bytes[0], mac->addr_bytes[1],
             mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5], queue_id);

    return flow;
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