/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <stdint.h>

#include "../include/tas.h"
#include "src/config/config.h"
#include "src/eth/eth.h"

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

#define INVALID_PORT_ID 0xFF

eth_state_t eth = {
    .num_queues = 0,
    .vlan_tags =
        {
            1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
            1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
            1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
            1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
        },
};

/* Regular configuration for NICs */
static struct rte_eth_conf non_vmdq_conf_default = {
    .rxmode =
        {
            .mq_mode = ETH_MQ_RX_NONE,
            .split_hdr_size = 0,
            .offloads = DEV_RX_OFFLOAD_VLAN_STRIP,
        },
    .txmode =
        {
            .mq_mode = ETH_MQ_TX_NONE,
            .offloads = (DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_TCP_CKSUM | DEV_TX_OFFLOAD_VLAN_INSERT |
                         DEV_TX_OFFLOAD_MULTI_SEGS | DEV_TX_OFFLOAD_TCP_TSO),
        },
};

uint32_t port = 0;
static unsigned num_threads;

/*
 * Initialises a given port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
int port_init(uint16_t n_threads) {
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf port_conf;
    struct rte_eth_rxconf *rxconf;
    struct rte_eth_txconf *txconf;
    int16_t rx_rings, tx_rings;
    uint16_t rx_ring_size, tx_ring_size;
    int retval;
    uint16_t q;
    uint32_t portid;

    num_threads = n_threads; // no. of fastpath cores
    RTE_ETH_FOREACH_DEV(portid) {
        if ((config.enable_port_mask & (1 << portid)) == 0) {
            RTE_LOG(INFO, VHOST_PORT, "Skipping disabled port %d\n", portid);
            continue;
        }
        port = portid;
        break;
    }

    /* The max pool number from dev_info will be used to validate the pool number specified in cmd line */
    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        RTE_LOG(ERR, VHOST_PORT, "Error during getting device (port %u) info: %s\n", port, strerror(-retval));

        return retval;
    }
    // real run on xl170
    /* Use a reasonable default number of devices */
    eth.num_devices = 64; /* Default to 64 devices */

    rxconf = &dev_info.default_rxconf;
    txconf = &dev_info.default_txconf;
    rxconf->rx_drop_en = 1; // Enables RX drop when no buffers available (prevents blocking)

    rx_ring_size = RTE_TEST_RX_DESC_DEFAULT;
    tx_ring_size = RTE_TEST_TX_DESC_DEFAULT;

    /*
     * When dequeue zero copy is enabled, guest Tx used vring will be
     * updated only when corresponding mbuf is freed. Thus, the nb_tx_desc
     * (tx_ring_size here) must be small enough so that the driver will
     * hit the free threshold easily and free mbufs timely. Otherwise,
     * guest Tx vring would be starved.
     */
    if (config.dequeue_zero_copy)
        tx_ring_size = 64;

    tx_rings = (uint16_t)rte_lcore_count(); // one TX queue per core

    /* Get regular port configuration. */
    port_conf = non_vmdq_conf_default;
    /* Use available RX queues, limit to what NIC supports */
    eth.queues_per_pool = 1;
    eth.num_pf_queues = 0;
    /* Limit num_devices to available RX queues */
    if (eth.num_devices > dev_info.max_rx_queues)
        eth.num_devices = dev_info.max_rx_queues;
    eth.num_queues = eth.num_devices;
    printf("Non-VMDq mode: configured %u devices, 1 queue per device\n", eth.num_devices);

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    /* Limit rx_rings to what we actually need and what the NIC supports */
    /* In non-VMDq mode, use only the queues we need */
    rx_rings = (uint16_t)eth.num_queues;
    if (rx_rings > dev_info.max_rx_queues)
        rx_rings = (uint16_t)dev_info.max_rx_queues;

    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    /* Configure ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0) {
        RTE_LOG(ERR, VHOST_PORT, "Failed to configure port %u: %s.\n", port, strerror(-retval));
        return retval;
    }

    // Adjusts descriptor counts to NIC-supported values (may be reduced)
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_ring_size, &tx_ring_size);
    if (retval != 0) {
        RTE_LOG(ERR, VHOST_PORT,
                "Failed to adjust number of descriptors "
                "for port %u: %s.\n",
                port, strerror(-retval));
        return retval;
    }
    if (rx_ring_size > RTE_TEST_RX_DESC_DEFAULT) {
        RTE_LOG(ERR, VHOST_PORT,
                "Mbuf pool has an insufficient size "
                "for Rx queues on port %u.\n",
                port);
        return -1;
    }

    /* Setup the queues. */
    // NIC hardware queues (RX/TX rings on the physical NIC), not virtqueues
    rxconf->offloads = port_conf.rxmode.offloads;
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, rx_ring_size, rte_eth_dev_socket_id(port), rxconf, eth.mbuf_pool);
        if (retval < 0) {
            RTE_LOG(ERR, VHOST_PORT, "Failed to setup rx queue %u of port %u: %s.\n", q, port, strerror(-retval));
            return retval;
        }
    }
    txconf->offloads = port_conf.txmode.offloads;
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, tx_ring_size, rte_eth_dev_socket_id(port), txconf);
        if (retval < 0) {
            RTE_LOG(ERR, VHOST_PORT, "Failed to setup tx queue %u of port %u: %s.\n", q, port, strerror(-retval));
            return retval;
        }
    }

    /* Start the device. */
    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        RTE_LOG(ERR, VHOST_PORT, "Failed to start port %u: %s\n", port, strerror(-retval));
        return retval;
    }

    if (config.promiscuous) {
        retval = rte_eth_promiscuous_enable(port);
        if (retval != 0) {
            RTE_LOG(ERR, VHOST_PORT, "Failed to enable promiscuous mode on port %u: %s\n", port, rte_strerror(-retval));
            return retval;
        }
    }

    RTE_LOG(INFO, VHOST_PORT, "Max virtio devices supported: %u\n", eth.num_devices);

    return 0;
}

/*
 * Update the global var NUM_PORTS and array PORTS according to system ports number
 * and return valid ports number
 */
unsigned check_ports_num(unsigned nb_ports) {
    unsigned valid_num_ports = config.num_ports;
    unsigned portid;

    if (config.num_ports > nb_ports) {
        RTE_LOG(INFO, VHOST_PORT, "\nSpecified port number(%u) exceeds total system port number(%u)\n",
                config.num_ports, nb_ports);
        config.num_ports = nb_ports;
    }

    for (portid = 0; portid < config.num_ports; portid++) {
        if (!rte_eth_dev_is_valid_port(config.ports[portid])) {
            RTE_LOG(INFO, VHOST_PORT, "\nSpecified port ID(%u) is not valid\n", config.ports[portid]);
            config.ports[portid] = INVALID_PORT_ID;
            valid_num_ports--;
        }
    }
    return valid_num_ports;
}