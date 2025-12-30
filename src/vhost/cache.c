#include "log.h"
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_jhash.h>

struct rte_hash *mac_2_vid = NULL;
struct rte_hash *ip_2_vid = NULL;

#define MAC_LOOKUP_TABLE_SIZE 256

int init_mac_2_vid() {
    struct rte_hash_parameters hash_params = {
        .name = "mac_2_vid",
        .entries = MAC_LOOKUP_TABLE_SIZE,
        .key_len = sizeof(struct rte_ether_addr),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };

    mac_2_vid = rte_hash_create(&hash_params);
    if (mac_2_vid == NULL) {
        LOG_ERROR("Failed to create MAC lookup hash table\n");
        return -1;
    }

    LOG_INFO("MAC lookup hash table initialized (size=%d)\n", MAC_LOOKUP_TABLE_SIZE);
    return 0;
}

int destroy_mac_2_vid() {
    // Cleanup MAC lookup hash table
    if (mac_2_vid != NULL) {
        rte_hash_free(mac_2_vid);
        mac_2_vid = NULL;
        LOG_INFO("MAC lookup hash table destroyed\n");
    }
    return 0;
}