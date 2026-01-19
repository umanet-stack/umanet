/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#ifndef CONFIG_H
#define CONFIG_H
#include <rte_build_config.h>
#include <rte_ether.h>
#include <stdint.h>

typedef struct {
    /* ===== vhost-user ===== */
    uint32_t client_mode;
    uint32_t dequeue_zero_copy;
    uint32_t mergeable;
    /* Enable retries on RX. */
    uint32_t enable_retry;
    /* Disable TX checksum offload */
    uint32_t enable_tx_csum;
    /* Disable TSO offload */
    uint32_t enable_tso;
    /* Specify timeout (in useconds) between retries on RX. */
    uint32_t burst_rx_delay_time;
    /* Specify the number of retries on RX. */
    uint32_t burst_rx_retry_num;
    // directory to store socket files
    char *socket_dir;
    /* Socket file paths e.g. {socket_dir}/sock0, {socket_dir}/sock1, ... */
    char *socket_files;
    uint32_t nb_sockets;
    /** IP address for this host */
    uint32_t ip;
    /** IP prefix length for this host */
    uint8_t ip_prefix;
    // mac address for this host
    struct rte_ether_addr mac;
    // mac address of other node nic
    struct rte_ether_addr other_node_mac;
    uint16_t eth_rx_cores;
    uint16_t eth_tx_cores;
    // no. of total nic tx queues that eth rx will poll from
    uint16_t eth_rx_queues;
    // no. of total nic rx queues that eth tx will send to
    uint16_t eth_tx_queues;
    uint16_t vhost_rx_cores;
    uint16_t vhost_tx_cores;
    uint8_t show_dash;
} config_t;

void init_config(config_t *c);
int parse_config(config_t *c, int argc, char **argv);

#endif /* CONFIG_H */