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
    uint32_t enable_stats;
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
    /* Socket file paths. Can be set by user */
    char *socket_files;
    int nb_sockets;
    /** IP address for this host */
    uint32_t ip;
    /** IP prefix length for this host */
    uint8_t ip_prefix;
    // mac address for this host
    struct rte_ether_addr mac;

    /* ===== TAS ===== */
    /* shared memory size */
    uint64_t shm_len;

    /** FP: maximal number of cores used */
    uint32_t fp_cores_max;
    /** FP: interrupts (blocking) enabled */
    uint32_t fp_interrupts;
    /** FP: tcp checksum offload enabled */
    uint32_t fp_xsumoffload;
    /** FP: auto scaling enabled */
    uint32_t fp_autoscale;
    /** FP: use huge pages for internal and buffer memory */
    uint32_t fp_hugepages;
    /** FP: enable vlan stripping */
    uint32_t fp_vlan_strip;
    /** FP: polling interval for TAS */
    uint32_t fp_poll_interval_tas;
    /** FP: polling interval for app */
    uint32_t fp_poll_interval_app;
} config_t;

void init_config(config_t *c);
int parse_config(config_t *c, int argc, char **argv);

#endif /* CONFIG_H */