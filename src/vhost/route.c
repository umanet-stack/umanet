#include "log.h"
#include "src/vhost/vhost.h"
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_jhash.h>

// Global route table
int ip_last_octet_to_vid[256];

void init_route_table() {
    for (int i = 0; i < 256; i++) {
        ip_last_octet_to_vid[i] = -1;
    }
    return;
}

int add_route_entry(int vid, uint32_t ip) {
    if (ip_last_octet_to_vid[ip & 0xFF] != -1) {
        LOG_WARN("Route entry already exists for IP %u.%u.%u.%u\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                 (ip >> 8) & 0xff, ip & 0xff);
        return -1;
    }
    ip_last_octet_to_vid[ip & 0xFF] = vid;
    LOG_INFO("Added route entry for IP %u.%u.%u.%u (vid=%d)", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
             ip & 0xff, vid);
    return 0;
}

int remove_route_entry(uint32_t ip) {
    if (ip_last_octet_to_vid[ip & 0xFF] == -1) {
        LOG_WARN("Route entry not found for IP %u.%u.%u.%u\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
                 ip & 0xff);
        return -1; // not found
    }
    ip_last_octet_to_vid[ip & 0xFF] = -1;
    LOG_INFO("Removed route entry for IP %u.%u.%u.%u\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
             ip & 0xff);
    return 0;
}

// struct route_table route_table = {
//     .mac_2_vid = NULL,
//     .ip_2_vid = NULL,
// };

// #define MAC_LOOKUP_TABLE_SIZE 256
// #define IP_LOOKUP_TABLE_SIZE 256

// int init_route_table() {
//     struct rte_hash *mac_2_vid = NULL;
//     struct rte_hash *ip_2_vid = NULL;

//     struct rte_hash_parameters hash_params = {
//         .name = "mac_2_vid",
//         .entries = MAC_LOOKUP_TABLE_SIZE,
//         .key_len = sizeof(struct rte_ether_addr),
//         .hash_func = rte_jhash,
//         .hash_func_init_val = 0,
//         .socket_id = rte_socket_id(),
//     };

//     mac_2_vid = rte_hash_create(&hash_params);
//     if (mac_2_vid == NULL) {
//         LOG_ERROR("Failed to create MAC lookup hash table\n");
//         return -1;
//     }
//     LOG_INFO("MAC lookup hash table initialized (size=%d)\n", MAC_LOOKUP_TABLE_SIZE);

//     hash_params.name = "ip_2_vid";
//     hash_params.entries = IP_LOOKUP_TABLE_SIZE;
//     hash_params.key_len = sizeof(uint32_t);

//     ip_2_vid = rte_hash_create(&hash_params);
//     if (ip_2_vid == NULL) {
//         LOG_ERROR("Failed to create IP lookup hash table\n");
//         return -1;
//     }
//     LOG_INFO("IP lookup hash table initialized (size=%d)\n", IP_LOOKUP_TABLE_SIZE);

//     route_table.mac_2_vid = mac_2_vid;
//     route_table.ip_2_vid = ip_2_vid;
//     return 0;
// }

// int cleanup_route_table() {
//     if (route_table.mac_2_vid != NULL) {
//         rte_hash_free(route_table.mac_2_vid);
//         route_table.mac_2_vid = NULL;
//         LOG_INFO("MAC lookup hash table destroyed\n");
//     }
//     if (route_table.ip_2_vid != NULL) {
//         rte_hash_free(route_table.ip_2_vid);
//         route_table.ip_2_vid = NULL;
//         LOG_INFO("IP lookup hash table destroyed\n");
//     }
//     return 0;
// }

// int add_route_entry(int vid, struct rte_ether_addr *mac, uint32_t ip) {
//     int ret = rte_hash_add_key_data(route_table.mac_2_vid, mac, (void *)(uintptr_t)vid);
//     if (ret < 0) {
//         LOG_ERROR("Failed to add route entry for MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac->addr_bytes[0],
//                   mac->addr_bytes[1], mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4],
//                   mac->addr_bytes[5]);
//         return -1;
//     }
//     ret = rte_hash_add_key_data(route_table.ip_2_vid, &ip, (void *)(uintptr_t)vid);
//     if (ret < 0) {
//         LOG_ERROR("Failed to add route entry for IP %u.%u.%u.%u\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
//                   (ip >> 8) & 0xff, ip & 0xff);
//         return -1;
//     }

//     LOG_INFO("(%d) Added route entry for MAC %02x:%02x:%02x:%02x:%02x:%02x and IP %u.%u.%u.%u\n", vid,
//              mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4],
//              mac->addr_bytes[5], (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
//     return 0;
// }

// int remove_route_entry(int vid, struct rte_ether_addr *mac, uint32_t ip) {
//     int ret = rte_hash_del_key(route_table.mac_2_vid, mac);
//     if (ret < 0 && ret != -ENOENT) {
//         LOG_ERROR("Failed to remove route entry for MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac->addr_bytes[0],
//                   mac->addr_bytes[1], mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4],
//                   mac->addr_bytes[5]);
//         return -1;
//     }
//     ret = rte_hash_del_key(route_table.ip_2_vid, &ip);
//     if (ret < 0 && ret != -ENOENT) {
//         LOG_ERROR("Failed to remove route entry for IP %u.%u.%u.%u\n", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
//                   (ip >> 8) & 0xff, ip & 0xff);
//         return -1;
//     }

//     LOG_INFO("(%d) Removed route entry for MAC %02x:%02x:%02x:%02x:%02x:%02x and IP %u.%u.%u.%u\n", vid,
//              mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2], mac->addr_bytes[3], mac->addr_bytes[4],
//              mac->addr_bytes[5], (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
//     return 0;
// }

// int find_vid_by_mac(struct rte_ether_addr *mac) {
//     void *data;
//     int ret = rte_hash_lookup_data(route_table.mac_2_vid, mac, &data);
//     if (ret >= 0) {
//         return (int)(uintptr_t)data; // Cast pointer back to int (vid was stored as (void *)(uintptr_t)vid)
//     }
//     return -1;
// }
