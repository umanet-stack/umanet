#include "main.h"
#include <rte_ethdev.h>
#include <stdint.h>
#include <sys/queue.h>

#ifndef MAX_QUEUES
#define MAX_QUEUES 128
#endif

/* the maximum number of external ports supported */
#define MAX_SUP_PORTS 1

#define MBUF_CACHE_SIZE 128
#define MBUF_DATA_SIZE RTE_MBUF_DEFAULT_BUF_SIZE

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

#define BURST_RX_WAIT_US 15 /* Defines how long we wait between retries on RX */
#define BURST_RX_RETRIES 4  /* Number of retries on RX. */

#define JUMBO_FRAME_MAX_SIZE 0x2600

/* State of virtio device. */
#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

#define INVALID_PORT_ID 0xFF

/* Maximum long option length for option parsing. */
#define MAX_LONG_OPT_SZ 64

/* Enable VM2VM communications. If this is disabled then the MAC address compare is skipped. */
typedef enum { VM2VM_DISABLED = 0, VM2VM_SOFTWARE = 1, VM2VM_HARDWARE = 2, VM2VM_LAST } vm2vm_type;

/* empty vmdq configuration structure. Filled in programatically */
static struct rte_eth_conf vmdq_conf_default = {
    .rxmode =
        {
            .mq_mode = ETH_MQ_RX_VMDQ_ONLY,
            .split_hdr_size = 0,
            /*
             * VLAN strip is necessary for 1G NIC such as I350,
             * this fixes bug of ipv4 forwarding in guest can't
             * forward pakets from one virtio dev to another virtio dev.
             */
            .offloads = DEV_RX_OFFLOAD_VLAN_STRIP,
        },
};

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

const uint16_t vlan_tags[] = {
    1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
    1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
    1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
    1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
};

typedef struct AppState {
    uint32_t enabled_port_mask;
    uint32_t promiscuous;
    uint32_t num_queues;
    uint32_t num_devices;

    struct rte_mempool *mbuf_pool;
    int mergeable;
    vm2vm_type vm2vm_mode;

    /* Enable stats. */
    uint32_t enable_stats;
    /* Enable retries on RX. */
    uint32_t enable_retry;
    /* Disable TX checksum offload */
    uint32_t enable_tx_csum;
    /* Disable TSO offload */
    uint32_t enable_tso;

    int client_mode;
    int dequeue_zero_copy;
    int builtin_net_driver;

    /* Specify timeout (in useconds) between retries on RX. */
    uint32_t burst_rx_delay_time;
    /* Specify the number of retries on RX. */
    uint32_t burst_rx_retry_num;

    /* Socket file paths. Can be set by user */
    char *socket_files;
    int nb_sockets;

    unsigned lcore_ids[RTE_MAX_LCORE];
    uint16_t ports[RTE_MAX_ETHPORTS];
    unsigned num_ports; /**< The number of ports specified in command line */
    uint16_t num_pf_queues;
    uint16_t num_vmdq_queues;
    uint16_t vmdq_pool_base, vmdq_queue_base;
    uint16_t queues_per_pool;
    int vmdq_enabled; /* Flag to indicate if VMDq is available */

    /* ethernet addresses of ports */
    struct rte_ether_addr vmdq_ports_eth_addr[RTE_MAX_ETHPORTS];

    struct vhost_dev_tailq_list vhost_dev_list;
    struct lcore_info lcore_info[RTE_MAX_LCORE];

} AppState;

extern AppState *app_state;
