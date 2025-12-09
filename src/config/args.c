#include <bits/getopt_core.h>
#include <bits/getopt_ext.h>
#include <rte_memory.h>

static int client_mode;
static int dequeue_zero_copy;
static int builtin_net_driver;

/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;

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
 * Parse the arguments given in the command line of the application.
 */
static int us_vhost_parse_args(int argc, char **argv) {
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
        {"client", no_argument, &client_mode, 1},
        {"dequeue-zero-copy", no_argument, &dequeue_zero_copy, 1},
        {"builtin-net-driver", no_argument, &builtin_net_driver, 1},
        {NULL, 0, 0, 0},
    };

    /* Parse command line */
    while ((opt = getopt_long(argc, argv, "p:P", long_option, &option_index)) != EOF) {
        switch (opt) {
        /* Portmask */
        case 'p':
            enabled_port_mask = parse_portmask(optarg);
            if (enabled_port_mask == 0) {
                RTE_LOG(INFO, VHOST_CONFIG, "Invalid portmask\n");
                us_vhost_usage(prgname);
                return -1;
            }
            break;

        case 'P':
            promiscuous = 1;
            vmdq_conf_default.rx_adv_conf.vmdq_rx_conf.rx_mode = ETH_VMDQ_ACCEPT_BROADCAST | ETH_VMDQ_ACCEPT_MULTICAST;

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
                    vm2vm_mode = (vm2vm_type)ret;
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
                    enable_retry = ret;
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
                    enable_tx_csum = ret;
            }

            /* Enable/disable TSO offload. */
            if (!strncmp(long_option[option_index].name, "tso", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, 1);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for tso [0|1]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else
                    enable_tso = ret;
            }

            /* Specify the retries delay time (in useconds) on RX. */
            if (!strncmp(long_option[option_index].name, "rx-retry-delay", MAX_LONG_OPT_SZ)) {
                ret = parse_num_opt(optarg, INT32_MAX);
                if (ret == -1) {
                    RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for rx-retry-delay [0-N]\n");
                    us_vhost_usage(prgname);
                    return -1;
                } else {
                    burst_rx_delay_time = ret;
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
                    burst_rx_retry_num = ret;
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
                    mergeable = !!ret;
                    if (ret) {
                        vmdq_conf_default.rxmode.offloads |= DEV_RX_OFFLOAD_JUMBO_FRAME;
                        vmdq_conf_default.rxmode.max_rx_pkt_len = JUMBO_FRAME_MAX_SIZE;
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
                    enable_stats = ret;
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
        if (enabled_port_mask & (1 << i))
            ports[num_ports++] = i;
    }

    if ((num_ports == 0) || (num_ports > MAX_SUP_PORTS)) {
        RTE_LOG(INFO, VHOST_PORT,
                "Current enabled port number is %u,"
                "but only %u port can be enabled\n",
                num_ports, MAX_SUP_PORTS);
        return -1;
    }

    return 0;
}