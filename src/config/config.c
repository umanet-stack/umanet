// /* SPDX-License-Identifier: BSD-3-Clause
//  * Copyright(c) 2010-2017 Intel Corporation
//  */

// #include <rte_ethdev.h>
// #include <rte_vhost.h>
// #include <stdint.h>

// #include "constants.h"
// #include "main.h"

// /* mask of enabled ports */
// static uint32_t enabled_port_mask = 0;

// /* Promiscuous mode */
// static uint32_t promiscuous;

// /* number of devices/queues to support*/
// static uint32_t num_queues = 0;
// static uint32_t num_devices;

// static struct rte_mempool *mbuf_pool;
// static int mergeable;

// /* Enable VM2VM communications. If this is disabled then the MAC address compare is skipped. */
// typedef enum { VM2VM_DISABLED = 0, VM2VM_SOFTWARE = 1, VM2VM_HARDWARE = 2, VM2VM_LAST } vm2vm_type;
// static vm2vm_type vm2vm_mode = VM2VM_SOFTWARE;

// /* Enable stats. */
// static uint32_t enable_stats = 0;
// /* Enable retries on RX. */
// static uint32_t enable_retry = 1;

// /* Disable TX checksum offload */
// static uint32_t enable_tx_csum;

// /* Disable TSO offload */
// static uint32_t enable_tso;

// static int client_mode;
// static int dequeue_zero_copy;

// static int builtin_net_driver;

// /* Specify timeout (in useconds) between retries on RX. */
// static uint32_t burst_rx_delay_time = BURST_RX_WAIT_US;
// /* Specify the number of retries on RX. */
// static uint32_t burst_rx_retry_num = BURST_RX_RETRIES;

// /* Socket file paths. Can be set by user */
// static char *socket_files;
// static int nb_sockets;

// /* empty vmdq configuration structure. Filled in programatically */
// static struct rte_eth_conf vmdq_conf_default = {
//     .rxmode =
//         {
//             .mq_mode = ETH_MQ_RX_VMDQ_ONLY,
//             .split_hdr_size = 0,
//             /*
//              * VLAN strip is necessary for 1G NIC such as I350,
//              * this fixes bug of ipv4 forwarding in guest can't
//              * forward pakets from one virtio dev to another virtio dev.
//              */
//             .offloads = DEV_RX_OFFLOAD_VLAN_STRIP,
//         },
// };

// /* Non-VMDq configuration for NICs without VMDq support */
// static struct rte_eth_conf non_vmdq_conf_default = {
//     .rxmode =
//         {
//             .mq_mode = ETH_MQ_RX_NONE,
//             .split_hdr_size = 0,
//             .offloads = DEV_RX_OFFLOAD_VLAN_STRIP,
//         },
//     .txmode =
//         {
//             .mq_mode = ETH_MQ_TX_NONE,
//             .offloads = (DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_TCP_CKSUM | DEV_TX_OFFLOAD_VLAN_INSERT |
//                          DEV_TX_OFFLOAD_MULTI_SEGS | DEV_TX_OFFLOAD_TCP_TSO),
//         },
// };

// static unsigned lcore_ids[RTE_MAX_LCORE];
// static uint16_t ports[RTE_MAX_ETHPORTS];
// static unsigned num_ports = 0; /**< The number of ports specified in command line */
// static uint16_t num_pf_queues, num_vmdq_queues;
// static uint16_t vmdq_pool_base, vmdq_queue_base;
// static uint16_t queues_per_pool;
// static int vmdq_enabled = 0; /* Flag to indicate if VMDq is available */

// const uint16_t vlan_tags[] = {
//     1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
//     1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
//     1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
//     1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
// };

// /* ethernet addresses of ports */
// static struct rte_ether_addr vmdq_ports_eth_addr[RTE_MAX_ETHPORTS];

// static struct vhost_dev_tailq_list vhost_dev_list = TAILQ_HEAD_INITIALIZER(vhost_dev_list);

// static struct lcore_info lcore_info[RTE_MAX_LCORE];

// /* Used for queueing bursts of TX packets. */
// struct mbuf_table {
//     unsigned len;
//     unsigned txq_id;
//     struct rte_mbuf *m_table[MAX_PKT_BURST];
// };

// /* TX queue for each data core. */
// struct mbuf_table lcore_tx_queue[RTE_MAX_LCORE];

// #define MBUF_TABLE_DRAIN_TSC ((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US)
// #define VLAN_HLEN 4