#include <rte_ethdev.h>
#include <stdint.h>

#include "main.h"
#include "src/config/config.h"

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

#define INVALID_PORT_ID 0xFF

static int vmdq_enabled = 0; /* Flag to indicate if VMDq is available */

/* number of devices/queues to support*/
static uint32_t num_queues = 0;
static uint32_t num_devices;

static struct rte_mempool *mbuf_pool;

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

static uint16_t num_pf_queues, num_vmdq_queues;
static uint16_t vmdq_pool_base, vmdq_queue_base;
static uint16_t queues_per_pool;

const uint16_t vlan_tags[] = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
    1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
    1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
    1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
};

/* ethernet addresses of ports */
static struct rte_ether_addr vmdq_ports_eth_addr[RTE_MAX_ETHPORTS];

/*
 * Builds up the correct configuration for VMDQ VLAN pool map
 * according to the pool & queue limits.
 */
static inline int get_eth_conf(struct rte_eth_conf *eth_conf, uint32_t num_devices) {
    struct rte_eth_vmdq_rx_conf conf;
    struct rte_eth_vmdq_rx_conf *def_conf = // default config
        &config.vmdq_conf_default->rx_adv_conf.vmdq_rx_conf;
    unsigned i;

    memset(&conf, 0, sizeof(conf));                           // Zero-initializes conf
    conf.nb_queue_pools = (enum rte_eth_nb_pools)num_devices; // Each pool serves one virtio device
    // queue pool = shared pool of queues
    conf.nb_pool_maps = num_devices; // One mapping per device
    conf.enable_loop_back = def_conf->enable_loop_back;
    conf.rx_mode = def_conf->rx_mode; // accept/broadcast/multicast

    for (i = 0; i < conf.nb_pool_maps; i++) {
        conf.pool_map[i].vlan_id = vlan_tags[i]; // (1000, 1001, ...)
        conf.pool_map[i].pools = (1UL << i);     // each pool accepts from 1 vlan tag
    }

    // Copies base config
    // (void) suppresses unused return value warning
    (void)(rte_memcpy(eth_conf, &config.vmdq_conf_default, sizeof(*eth_conf)));
    // Overwrites the VMDq section with the computed conf
    (void)(rte_memcpy(&eth_conf->rx_adv_conf.vmdq_rx_conf, &conf, sizeof(eth_conf->rx_adv_conf.vmdq_rx_conf)));
    return 0;
}

/*
 * Initialises a given port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
inline int port_init(uint16_t port) {
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
        RTE_LOG(ERR, VHOST_PORT, "Error during getting device (port %u) info: %s\n", port, strerror(-retval));

        return retval;
    }
    /* Check if VMDq is supported */
    if (dev_info.max_vmdq_pools == 0) {
        // real run on xl170, VMDq is not supported
        RTE_LOG(INFO, VHOST_PORT, "VMDq not supported, using non-VMDq mode.\n");
        vmdq_enabled = 0;
        /* Use a reasonable default number of devices when VMDq is not available */
        num_devices = 64; /* Default to 64 devices */
    } else {
        vmdq_enabled = 1;
        /*configure the number of supported virtio devices based on VMDQ limits */
        num_devices = dev_info.max_vmdq_pools;
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
    if (config.dequeue_zero_copy)
        tx_ring_size = 64;

    tx_rings = (uint16_t)rte_lcore_count(); // one TX queue per core

    /* Get port configuration. */
    if (vmdq_enabled) {
        retval = get_eth_conf(&port_conf, num_devices);
        if (retval < 0)
            return retval;
        /* NIC queues are divided into pf (physical function) queues and vmdq queues.  */
        num_pf_queues = dev_info.max_rx_queues - dev_info.vmdq_queue_num;
        queues_per_pool = dev_info.vmdq_queue_num / dev_info.max_vmdq_pools;
        num_vmdq_queues = num_devices * queues_per_pool;
        num_queues = num_pf_queues + num_vmdq_queues;
        vmdq_queue_base = dev_info.vmdq_queue_base;
        vmdq_pool_base = dev_info.vmdq_pool_base;
        printf("pf queue num: %u, configured vmdq pool num: %u, each vmdq pool has %u queues\n", num_pf_queues,
               num_devices, queues_per_pool);
    } else {
        /* Non-VMDq mode: use regular configuration */
        port_conf = non_vmdq_conf_default;
        /* Use available RX queues, limit to what NIC supports */
        queues_per_pool = 1;
        num_pf_queues = 0;
        /* Limit num_devices to available RX queues */
        if (num_devices > dev_info.max_rx_queues)
            num_devices = dev_info.max_rx_queues;
        num_vmdq_queues = num_devices;
        num_queues = num_devices;
        vmdq_queue_base = 0;
        vmdq_pool_base = 0;
        printf("Non-VMDq mode: configured %u devices, 1 queue per device\n", num_devices);
    }

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    /* Limit rx_rings to what we actually need and what the NIC supports */
    if (vmdq_enabled) {
        rx_rings = (uint16_t)dev_info.max_rx_queues;
    } else {
        /* In non-VMDq mode, use only the queues we need */
        rx_rings = (uint16_t)num_queues;
        if (rx_rings > dev_info.max_rx_queues)
            rx_rings = (uint16_t)dev_info.max_rx_queues;
    }
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
        retval = rte_eth_rx_queue_setup(port, q, rx_ring_size, rte_eth_dev_socket_id(port), rxconf, mbuf_pool);
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

    retval = rte_eth_macaddr_get(port, &vmdq_ports_eth_addr[port]);
    if (retval < 0) {
        RTE_LOG(ERR, VHOST_PORT, "Failed to get MAC address on port %u: %s\n", port, rte_strerror(-retval));
        return retval;
    }

    RTE_LOG(INFO, VHOST_PORT, "Max virtio devices supported: %u\n", num_devices);
    RTE_LOG(INFO, VHOST_PORT,
            "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n", port,
            vmdq_ports_eth_addr[port].addr_bytes[0], vmdq_ports_eth_addr[port].addr_bytes[1],
            vmdq_ports_eth_addr[port].addr_bytes[2], vmdq_ports_eth_addr[port].addr_bytes[3],
            vmdq_ports_eth_addr[port].addr_bytes[4], vmdq_ports_eth_addr[port].addr_bytes[5]);

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