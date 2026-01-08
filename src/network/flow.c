#include "log.h"
#include "src/include/main.h"
#include "src/vhost/vhost.h"
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <string.h>

// indexed by vm_id, not vid
struct rte_flow *eth_rx_flows[MAX_VHOSTS] = {NULL};

int install_eth_rx_flow(struct vhost_dev *vdev) {
    if (eth_rx_flows[vdev->vm_id] != NULL) {
        LOG_WARN("Flow already installed for VM %d\n", vdev->vm_id);
        return -1;
    }

    uint16_t eth_queue_id = vdev->vm_id % config.eth_rx_queues;

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

    memcpy(eth_spec.dst.addr_bytes, vdev->mac.addr_bytes, 6);
    memset(eth_mask.dst.addr_bytes, 0xff, 6);

    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    /* ---------- Actions ---------- */
    struct rte_flow_action actions[2];
    memset(actions, 0, sizeof(actions));

    struct rte_flow_action_queue queue = {
        .index = eth_queue_id,
    };

    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;

    /* ---------- Create ---------- */
    struct rte_flow_error error;
    struct rte_flow *flow = rte_flow_create(global->eth_port_id, &attr, pattern, actions, &error);

    if (!flow) {
        LOG_ERROR("Flow create failed (queue %u): %s\n", eth_queue_id, error.message ? error.message : "unknown");
        return -1;
    }

    eth_rx_flows[vdev->vm_id] = flow;
    LOG_INFO("Flow created for IP %u.%u.%u.%u (VM %d) mac=%02x:%02x:%02x:%02x:%02x:%02x, queue=%u\n",
             (vdev->ip >> 24) & 0xff, (vdev->ip >> 16) & 0xff, (vdev->ip >> 8) & 0xff, vdev->ip & 0xff, vdev->vm_id,
             vdev->mac.addr_bytes[0], vdev->mac.addr_bytes[1], vdev->mac.addr_bytes[2], vdev->mac.addr_bytes[3],
             vdev->mac.addr_bytes[4], vdev->mac.addr_bytes[5], eth_queue_id);

    return 0;
}

int uninstall_eth_rx_flow(struct vhost_dev *vdev) {
    struct rte_flow *flow = eth_rx_flows[vdev->vm_id];
    if (flow == NULL) {
        LOG_ERROR("Flow not found for VM %d\n", vdev->vm_id);
        return -1;
    }

    uint16_t eth_queue_id = vdev->vm_id % config.eth_rx_queues;
    struct rte_flow_error error;
    int ret = rte_flow_destroy(global->eth_port_id, flow, &error);
    if (ret != 0) {
        LOG_ERROR("Flow destroy failed (queue %u): %s\n", eth_queue_id, error.message ? error.message : "unknown");
        return -1;
    }

    return 0;
}