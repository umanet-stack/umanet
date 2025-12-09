#include "config.h"
#include "main.h"
#include <getopt.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_memory.h>

/* the maximum number of external ports supported */
#define MAX_SUP_PORTS 1

/* Maximum long option length for option parsing. */
#define MAX_LONG_OPT_SZ 64

#define BURST_RX_WAIT_US 15 /* Defines how long we wait between retries on RX */
#define BURST_RX_RETRIES 4  /* Number of retries on RX. */

#define JUMBO_FRAME_MAX_SIZE 0x2600

config_t config = {
    .enable_port_mask = 0,
    .vm2vm_mode = VM2VM_SOFTWARE,
    .enable_stats = 0,
    .enable_retry = 1,
    .burst_rx_delay_time = BURST_RX_WAIT_US,
    .burst_rx_retry_num = BURST_RX_RETRIES,
    .vmdq_conf_default =
        &(struct rte_eth_conf){
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
        },
    .num_ports = 0,
};

#define CFG config

/*
 * Parse the portmask provided at run time.
 */
static int parse_portmask(const char *portmask) // portmask e.g. 0x1
{
    char *end = NULL;
    unsigned long pm;

    errno = 0;

    /* parse hexadecimal string */
    pm = strtoul(portmask, &end, 16); // converts hexadecimal string to unsigned long
    if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0') || (errno != 0))
        return -1;

    if (pm == 0)
        return -1;

    return pm;
}

/*
 * Parse num options at run time.
 */
// Generic parser for numeric options with range validation.
static int parse_num_opt(const char *q_arg, uint32_t max_valid_value) {
    char *end = NULL;
    unsigned long num;

    errno = 0;

    /* parse unsigned int string */
    num = strtoul(q_arg, &end, 10);
    if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0') || (errno != 0))
        return -1;

    if (num > max_valid_value)
        return -1;

    return num;
}

/*
 * Display usage
 */
static void us_vhost_usage(const char *prgname) {
    RTE_LOG(INFO, VHOST_CONFIG,
            "%s [EAL options] -- -p PORTMASK\n"
            "		--vm2vm [0|1|2]\n"
            "		--rx_retry [0|1] --mergeable [0|1] --stats [0-N]\n"
            "		--socket-file <path>\n"
            "		--nb-devices ND\n"
            "		-p PORTMASK: Set mask for ports to be used by application\n"
            "		--vm2vm [0|1|2]: disable/software(default)/hardware vm2vm comms\n"
            "		--rx-retry [0|1]: disable/enable(default) retries on rx. Enable retry if destintation queue is "
            "full\n"
            "		--rx-retry-delay [0-N]: timeout(in usecond) between retries on RX. This makes effect only if "
            "retries on rx enabled\n"
            "		--rx-retry-num [0-N]: the number of retries on rx. This makes effect only if retries on rx "
            "enabled\n"
            "		--mergeable [0|1]: disable(default)/enable RX mergeable buffers\n"
            "		--stats [0-N]: 0: Disable stats, N: Time in seconds to print stats\n"
            "		--socket-file: The path of the socket file.\n"
            "		--tx-csum [0|1] disable/enable TX checksum offload.\n"
            "		--tso [0|1] disable/enable TCP segment offload.\n"
            "		--client register a vhost-user socket as client mode.\n"
            "		--dequeue-zero-copy enables dequeue zero copy\n",
            prgname);
}

/*
 * Set socket file path.
 */
static int us_vhost_parse_socket_path(const char *q_arg) // path e.g. /tmp/vhost-user.sock
{
    char *old;

    /* parse number string */
    if (strnlen(q_arg, PATH_MAX) == PATH_MAX) // check if path is too long
        return -1;

    old = CFG.socket_files;
    // Reallocates socket_files to fit one more socket path
    CFG.socket_files = realloc(CFG.socket_files, PATH_MAX * (CFG.nb_sockets + 1));
    if (CFG.socket_files == NULL) { // check if realloc failed
        free(old);
        return -1;
    }

    strlcpy(CFG.socket_files + CFG.nb_sockets * PATH_MAX, q_arg, PATH_MAX); // copies path to socket_files' new slot
    CFG.nb_sockets++;

    return 0;
}

/*
 * Parse the arguments given in the command line of the application.
 */
int us_vhost_parse_args(int argc, char **argv) {
    int opt, ret;
    int option_index;
    unsigned i;
    const char *prgname = argv[0];
    static struct option long_option[] = {
        {"vm2vm", required_argument, NULL, 0},
        {"rx-retry", required_argument, NULL, 0},
        {"rx-retry-delay", required_argument, NULL, 0},
        {"rx-retry-num", required_argument, NULL, 0},
        {"mergeable", required_argument, NULL, 0},
        {"stats", required_argument, NULL, 0},
        {"socket-file", required_argument, NULL, 0},
        {"tx-csum", required_argument, NULL, 0},
        {"tso", required_argument, NULL, 0},
        {"client", no_argument, &CFG.client_mode, 1},
        {"dequeue-zero-copy", no_argument, &CFG.dequeue_zero_copy, 1},
        {"builtin-net-driver", no_argument, &CFG.builtin_net_driver, 1},
        {NULL, 0, 0, 0},
    };

    /* Parse command line */
    while ((opt = getopt_long(argc, argv, "p:P", long_option, &option_index)) != EOF) {
        switch (opt) {
        /* Portmask */
        case 'p':
            CFG.enable_port_mask = parse_portmask(optarg);
            if (CFG.enable_port_mask == 0) {
                RTE_LOG(INFO, VHOST_CONFIG, "Invalid portmask\n");
                us_vhost_usage(prgname);
                return -1;
            }
            break;

        case 'P':
            CFG.promiscuous = 1;
            CFG.vmdq_conf_default->rx_adv_conf.vmdq_rx_conf.rx_mode =
                ETH_VMDQ_ACCEPT_BROADCAST | ETH_VMDQ_ACCEPT_MULTICAST;

            break;

        case 0:
            /* Enable/disable vm2vm comms. */
            if (!strncmp(long_option[option_index].name, "vm2vm", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, (VM2VM_LAST - 1));
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG,
                            "Invalid argument for "
                            "vm2vm [0|1|2]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.vm2vm_mode = (vm2vm_type)ret;
                }
            }

            /* Enable/disable retries on RX. */
            if (!strncmp(long_option[option_index].name, "rx-retry", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, 1);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for rx-retry [0|1]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.enable_retry = ret;
                }
            }

            /* Enable/disable TX checksum offload. */
            if (!strncmp(long_option[option_index].name, "tx-csum", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, 1);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for tx-csum [0|1]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else
                    CFG.enable_tx_csum = ret;
            }

            /* Enable/disable TSO offload. */
            if (!strncmp(long_option[option_index].name, "tso", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, 1);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for tso [0|1]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else
                    CFG.enable_tso = ret;
            }

            /* Specify the retries delay time (in useconds) on RX. */
            if (!strncmp(long_option[option_index].name, "rx-retry-delay", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, INT32_MAX);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for rx-retry-delay [0-N]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.burst_rx_delay_time = ret;
                }
            }

            /* Specify the retries number on RX. */
            if (!strncmp(long_option[option_index].name, "rx-retry-num", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, INT32_MAX);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for rx-retry-num [0-N]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.burst_rx_retry_num = ret;
                }
            }

            /* Enable/disable RX mergeable buffers. */
            if (!strncmp(long_option[option_index].name, "mergeable", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, 1);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for mergeable [0|1]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.mergeable = !!ret;
                    if (ret) {
                        CFG.vmdq_conf_default->rxmode.offloads |= DEV_RX_OFFLOAD_JUMBO_FRAME;
                        CFG.vmdq_conf_default->rxmode.max_rx_pkt_len = JUMBO_FRAME_MAX_SIZE;
                    }
                }
            }

            /* Enable/disable stats. */
            if (!strncmp(long_option[option_index].name, "stats", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, INT32_MAX);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for stats [0..N]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    CFG.enable_stats = ret;
                }
            }

            /* Set socket file path. */
            if (!strncmp(long_option[option_index].name, "socket-file", MAX_LONG_OPT_SZ)) {
                if (us_vhost_parse_socket_path(optarg) == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for socket name (Max %d characters)\n", PATH_MAX);
                    us_vhost_usage(prgname);
                    return -1;
                }
            }

            break;

            /* Invalid option - print options. */
        default:
            us_vhost_usage(prgname);
            return -1;
        }
    }

    for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
        if (CFG.enable_port_mask & (1 << i))
            CFG.ports[CFG.num_ports++] = i;
    }

    if ((CFG.num_ports == 0) || (CFG.num_ports > MAX_SUP_PORTS)) {
        RTE_LOG(INFO, VHOST_PORT,
                "Current enabled port number is %u,"
                "but only %u port can be enabled\n",
                CFG.num_ports, MAX_SUP_PORTS);
        return -1;
    }

    return 0;
}