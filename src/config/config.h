#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>

typedef enum { VM2VM_DISABLED = 0, VM2VM_SOFTWARE = 1, VM2VM_HARDWARE = 2, VM2VM_LAST } vm2vm_type;

typedef struct {
    int client_mode;
    int dequeue_zero_copy;
    int builtin_net_driver;

    /* mask of enabled ports */
    uint32_t enable_port_mask;

    /* Promiscuous mode */
    uint32_t promiscuous;
    int mergeable;

    vm2vm_type vm2vm_mode;
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

    /* empty vmdq configuration structure. Filled in programatically */
    struct rte_eth_conf *vmdq_conf_default;

    uint16_t ports[RTE_MAX_ETHPORTS];
    unsigned num_ports; /**< The number of ports specified in command line */
} config_t;

extern config_t config;
int us_vhost_parse_args(int argc, char **argv);

#endif /* CONFIG_H */