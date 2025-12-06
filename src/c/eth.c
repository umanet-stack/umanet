#include <rte_ethdev.h>
#include <rte_log.h>
#include <stdint.h>

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

/* number of devices/queues to support*/
static uint32_t num_queues = 0;
static uint32_t num_devices;

static int dequeue_zero_copy;
static uint16_t queues_per_pool;
static uint16_t vmdq_pool_base, vmdq_queue_base;
static uint16_t queues_per_pool;

/* Promiscuous mode */
static uint32_t promiscuous;

static struct rte_mempool *mbuf_pool;

/* ethernet addresses of ports */
static struct rte_ether_addr vmdq_ports_eth_addr[RTE_MAX_ETHPORTS];

/* Non-VMDq configuration for NICs without VMDq support */
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

/*
 * Initialises a given port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
static inline int port_init(uint16_t port) {
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf port_conf;
    struct rte_eth_rxconf *rxconf;
    struct rte_eth_txconf *txconf;
    int16_t rx_rings, tx_rings;
    uint16_t rx_ring_size, tx_ring_size;
    int retval;
    uint16_t q;

    /* The max pool number from dev_info will be used to validate the pool number specified in cmd line */
    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        RTE_LOG(ERR, EAL, "Error during getting device (port %u) info: %s\n", port, strerror(-retval));

        return retval;
    }
    /* Check if VMDq is supported */
    if (dev_info.max_vmdq_pools == 0) {
        // real run on xl170, VMDq is not supported
        RTE_LOG(INFO, EAL, "VMDq not supported, using non-VMDq mode.\n");
        // vmdq_enabled = 0;
        /* Use a reasonable default number of devices when VMDq is not available */
        num_devices = 64; /* Default to 64 devices */
    }

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
    if (dequeue_zero_copy)
        tx_ring_size = 64;

    tx_rings = (uint16_t)rte_lcore_count(); // one TX queue per core

    /* Get port configuration. */
    // if (vmdq_enabled) {
    //     retval = get_eth_conf(&port_conf, num_devices);
    //     if (retval < 0)
    //         return retval;
    //     /* NIC queues are divided into pf (physical function) queues and vmdq queues.  */
    //     num_pf_queues = dev_info.max_rx_queues - dev_info.vmdq_queue_num;
    //     queues_per_pool = dev_info.vmdq_queue_num / dev_info.max_vmdq_pools;
    //     num_vmdq_queues = num_devices * queues_per_pool;
    //     num_queues = num_pf_queues + num_vmdq_queues;
    //     vmdq_queue_base = dev_info.vmdq_queue_base;
    //     vmdq_pool_base = dev_info.vmdq_pool_base;
    //     printf("pf queue num: %u, configured vmdq pool num: %u, each vmdq pool has %u queues\n", num_pf_queues,
    //            num_devices, queues_per_pool);
    // } else {
    /* Non-VMDq mode: use regular configuration */
    port_conf = non_vmdq_conf_default;
    /* Use available RX queues, limit to what NIC supports */
    queues_per_pool = 1;
    // num_pf_queues = 0;
    /* Limit num_devices to available RX queues */
    if (num_devices > dev_info.max_rx_queues)
        num_devices = dev_info.max_rx_queues;
    // num_vmdq_queues = num_devices;
    num_queues = num_devices;
    vmdq_queue_base = 0;
    vmdq_pool_base = 0;
    printf("Non-VMDq mode: configured %u devices, 1 queue per device\n", num_devices);
    // }

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    /* Limit rx_rings to what we actually need and what the NIC supports */
    // if (vmdq_enabled) {
    //     rx_rings = (uint16_t)dev_info.max_rx_queues;
    // } else {
    /* In non-VMDq mode, use only the queues we need */
    rx_rings = (uint16_t)num_queues;
    if (rx_rings > dev_info.max_rx_queues)
        rx_rings = (uint16_t)dev_info.max_rx_queues;
    // }
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    /* Configure ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0) {
        fprintf(stderr, "Failed to configure port %u: %s.\n", port, strerror(-retval));
        return retval;
    }

    // Adjusts descriptor counts to NIC-supported values (may be reduced)
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_ring_size, &tx_ring_size);
    if (retval != 0) {
        fprintf(stderr, "Failed to adjust number of descriptors for port %u: %s.\n", port, strerror(-retval));
        return retval;
    }
    if (rx_ring_size > RTE_TEST_RX_DESC_DEFAULT) {
        fprintf(stderr, "Mbuf pool has an insufficient size for Rx queues on port %u.\n", port);
        return -1;
    }

    /* Setup the queues. */
    // NIC hardware queues (RX/TX rings on the physical NIC), not virtqueues
    rxconf->offloads = port_conf.rxmode.offloads;
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, rx_ring_size, rte_eth_dev_socket_id(port), rxconf, mbuf_pool);
        if (retval < 0) {
            fprintf(stderr, "Failed to setup rx queue %u of port %u: %s.\n", q, port, strerror(-retval));
            return retval;
        }
    }
    txconf->offloads = port_conf.txmode.offloads;
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, tx_ring_size, rte_eth_dev_socket_id(port), txconf);
        if (retval < 0) {
            fprintf(stderr, "Failed to setup tx queue %u of port %u: %s.\n", q, port, strerror(-retval));
            return retval;
        }
    }

    /* Start the device. */
    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        fprintf(stderr, "Failed to start port %u: %s\n", port, strerror(-retval));
        return retval;
    }

    if (promiscuous) {
        retval = rte_eth_promiscuous_enable(port);
        if (retval != 0) {
            fprintf(stderr, "Failed to enable promiscuous mode on port %u: %s\n", port, rte_strerror(-retval));
            return retval;
        }
    }

    retval = rte_eth_macaddr_get(port, &vmdq_ports_eth_addr[port]);
    if (retval < 0) {
        fprintf(stderr, "Failed to get MAC address on port %u: %s\n", port, rte_strerror(-retval));
        return retval;
    }

    fprintf(stdout, "Max virtio devices supported: %u\n", num_devices);
    fprintf(stdout, "Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", port, vmdq_ports_eth_addr[port].addr_bytes[0],
            vmdq_ports_eth_addr[port].addr_bytes[1], vmdq_ports_eth_addr[port].addr_bytes[2],
            vmdq_ports_eth_addr[port].addr_bytes[3], vmdq_ports_eth_addr[port].addr_bytes[4],
            vmdq_ports_eth_addr[port].addr_bytes[5]);

    return 0;
}