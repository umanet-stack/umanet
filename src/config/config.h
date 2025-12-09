// #ifndef CONFIG_H
// #define CONFIG_H

// #include "constants.h"
// #include <rte_ethdev.h>
// #include <rte_vhost.h>
// #include <stdint.h>

// typedef enum { VM2VM_DISABLED = 0, VM2VM_SOFTWARE = 1, VM2VM_HARDWARE = 2, VM2VM_LAST } vm2vm_type;

// struct config {
//     uint32_t enabled_port_mask;
//     uint32_t promiscuous;
//     uint32_t num_queues;
//     uint32_t num_devices;
//     struct rte_mempool *mbuf_pool;
//     int mergeable;
//     vm2vm_type vm2vm_mode;
//     uint32_t enable_stats;
//     uint32_t enable_retry;
//     uint32_t enable_tx_csum;
//     uint32_t enable_tso;
//     int client_mode;
//     int dequeue_zero_copy;
//     int builtin_net_driver;
//     uint32_t burst_rx_delay_time;
//     uint32_t burst_rx_retries;
//     char *socket_files;
//     int nb_sockets;
// };

// #endif /* CONFIG_H */