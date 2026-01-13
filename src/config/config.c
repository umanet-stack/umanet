/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include "utils.h"
#include <getopt.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <stdio.h>
#include <unistd.h>

#define BURST_RX_WAIT_US 15 /* Defines how long we wait between retries on RX */
#define BURST_RX_RETRIES 4  /* Number of retries on RX. */

static inline int parse_int8(const char *s, uint8_t *pi);
static inline int parse_int16(const char *s, uint16_t *pi);
static inline int parse_int32(const char *s, uint32_t *pi);
static int parse_socket_dir(config_t *c, const char *q_arg);
static inline int parse_cidr(char *s, uint32_t *ip, uint8_t *prefix);
static int parse_ether_addr(const char *s, struct rte_ether_addr *addr);

void init_config(config_t *c) {
    /* ===== vhost-user ===== */
    c->client_mode = 0;
    c->dequeue_zero_copy = 0;
    c->mergeable = 0;
    c->enable_retry = 1;
    c->enable_tx_csum = 0;
    c->enable_tso = 0;
    c->burst_rx_delay_time = BURST_RX_WAIT_US;
    c->burst_rx_retry_num = BURST_RX_RETRIES;
    c->socket_dir = NULL;
    c->socket_files = NULL;
    c->nb_sockets = 0;
    c->ip = 0;
    c->ip_prefix = 0;
    c->mac = (struct rte_ether_addr){{0x02, 0x00, 0x00, 0x00, 0x00, 0xFE}};
    c->other_node_mac = (struct rte_ether_addr){{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    c->eth_rx_cores = 0;
    c->eth_tx_cores = 0;
    c->eth_rx_queues = 0;
    c->eth_tx_queues = 0;
    c->vhost_rx_cores = 0;
    c->vhost_tx_cores = 0;
    c->show_dash = 0;
}

enum cfg_params {
    CP_PORTMASK,
    CP_RX_RETRY,
    CP_RX_RETRY_DELAY,
    CP_RX_RETRY_NUM,
    CP_MERGEABLE,
    CP_SOCKET_DIR,
    CP_NB_SOCKETS,
    CP_TX_CSUM,
    CP_TSO,
    CP_CLIENT,
    CP_DEQUEUE_ZERO_COPY,
    CP_IP_ADDR,
    CP_OTHER_NODE_MAC,
    CP_ETH_RX_CORES,
    CP_ETH_TX_CORES,
    CP_ETH_RX_QUEUES,
    CP_ETH_TX_QUEUES,
    CP_VHOST_RX_CORES,
    CP_VHOST_TX_CORES,
    CP_SHOW_DASH,
};

static struct option options[] = {
    {
        "portmask",
        required_argument,
        .val = CP_PORTMASK,
    },
    {
        "rx-retry",
        required_argument,
        .val = CP_RX_RETRY,
    },
    {
        "rx-retry-delay",
        required_argument,
        .val = CP_RX_RETRY_DELAY,
    },
    {
        "rx-retry-num",
        required_argument,
        .val = CP_RX_RETRY_NUM,
    },
    {
        "mergeable",
        required_argument,
        .val = CP_MERGEABLE,
    },
    {
        "socket-dir",
        required_argument,
        .val = CP_SOCKET_DIR,
    },
    {
        "nb-sockets",
        required_argument,
        .val = CP_NB_SOCKETS,
    },
    {
        "tx-csum",
        required_argument,
        .val = CP_TX_CSUM,
    },
    {
        "tso",
        required_argument,
        .val = CP_TSO,
    },
    {"client", no_argument, .val = CP_CLIENT},
    {"dequeue-zero-copy", no_argument, .val = CP_DEQUEUE_ZERO_COPY},
    {"ip-addr", required_argument, .val = CP_IP_ADDR},
    {"other-node-mac", required_argument, .val = CP_OTHER_NODE_MAC},
    {"eth-rx-cores", required_argument, .val = CP_ETH_RX_CORES},
    {"eth-tx-cores", required_argument, .val = CP_ETH_TX_CORES},
    {"eth-rx-queues", required_argument, .val = CP_ETH_RX_QUEUES},
    {"eth-tx-queues", required_argument, .val = CP_ETH_TX_QUEUES},
    {"vhost-rx-cores", required_argument, .val = CP_VHOST_RX_CORES},
    {"vhost-tx-cores", required_argument, .val = CP_VHOST_TX_CORES},
    {"show-dash", required_argument, .val = CP_SHOW_DASH},
};

static void us_vhost_usage(const char *prgname) {
    fprintf(stderr,
            "%s [EAL options] -- --portmask PORTMASK\n"
            "		--vm2vm [0|1|2]\n"
            "		--rx_retry [0|1] --mergeable [0|1] --stats [0-N]\n"
            "		--socket-file <path>\n"
            "		--nb-devices ND\n"
            "		--portmask PORTMASK: Set mask for ports to be used by application\n"
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

int parse_config(config_t *c, int argc, char **argv) {
    int opt;
    const char *prgname = argv[0];

    while ((opt = getopt_long(argc, argv, "", options, NULL)) != EOF) {
        switch (opt) {
        case CP_RX_RETRY:
            if (parse_int32(optarg, &c->enable_retry) != 0) {
                fprintf(stderr, "Invalid argument for rx-retry [0|1]\n");
                goto failed;
            }
            break;
        case CP_TX_CSUM:
            if (parse_int32(optarg, &c->enable_tx_csum) != 0) {
                fprintf(stderr, "Invalid argument for tx-csum [0|1]\n");
                goto failed;
            }
            break;
        case CP_TSO:
            if (parse_int32(optarg, &c->enable_tso) != 0) {
                fprintf(stderr, "Invalid argument for tso [0|1]\n");
                goto failed;
            }
            break;
        case CP_RX_RETRY_DELAY:
            if (parse_int32(optarg, &c->burst_rx_delay_time) != 0) {
                fprintf(stderr, "Invalid argument for rx-retry-delay [0-N]\n");
                goto failed;
            }
            break;
        case CP_RX_RETRY_NUM:
            if (parse_int32(optarg, &c->burst_rx_retry_num) != 0) {
                fprintf(stderr, "Invalid argument for rx-retry-num [0-N]\n");
                goto failed;
            }
            break;
        case CP_MERGEABLE:
            if (parse_int32(optarg, &c->mergeable) != 0) {
                fprintf(stderr, "Invalid argument for mergeable [0|1]\n");
                goto failed;
            }
            break;
        case CP_SOCKET_DIR:
            if (parse_socket_dir(c, optarg) == -1) {
                fprintf(stderr, "Invalid argument for socket directory (Max %d characters)\n", PATH_MAX);
                goto failed;
            }
            break;
        case CP_NB_SOCKETS:
            if (parse_int32(optarg, &c->nb_sockets) != 0) {
                fprintf(stderr, "Invalid argument for nb-sockets [0-N]\n");
                goto failed;
            }
            break;

        case CP_CLIENT:
            c->client_mode = 1;
            break;
        case CP_DEQUEUE_ZERO_COPY:
            c->dequeue_zero_copy = 1;
            break;

        case CP_IP_ADDR:
            if (parse_cidr(optarg, &c->ip, &c->ip_prefix) != 0) {
                fprintf(stderr, "Parsing IP failed\n");
                goto failed;
            }
            break;

        case CP_OTHER_NODE_MAC:
            if (parse_ether_addr(optarg, &c->other_node_mac) != 0) {
                fprintf(stderr, "Invalid argument for other-node-mac\n");
                goto failed;
            }
            break;

        case CP_ETH_RX_CORES:
            if (parse_int16(optarg, &c->eth_rx_cores) != 0) {
                fprintf(stderr, "Invalid argument for eth-rx-cores [0-N]\n");
                goto failed;
            }
            break;

        case CP_ETH_TX_CORES:
            if (parse_int16(optarg, &c->eth_tx_cores) != 0) {
                fprintf(stderr, "Invalid argument for eth-tx-cores [0-N]\n");
                goto failed;
            }
            break;

        case CP_ETH_RX_QUEUES:
            if (parse_int16(optarg, &c->eth_rx_queues) != 0) {
                fprintf(stderr, "Invalid argument for eth-rx-queues [0-N]\n");
                goto failed;
            }
            break;

        case CP_ETH_TX_QUEUES:
            if (parse_int16(optarg, &c->eth_tx_queues) != 0) {
                fprintf(stderr, "Invalid argument for eth-tx-queues [0-N]\n");
                goto failed;
            }
            break;

        case CP_VHOST_RX_CORES:
            if (parse_int16(optarg, &c->vhost_rx_cores) != 0) {
                fprintf(stderr, "Invalid argument for vhost-rx-cores [0-N]\n");
                goto failed;
            }
            break;

        case CP_VHOST_TX_CORES:
            if (parse_int16(optarg, &c->vhost_tx_cores) != 0) {
                fprintf(stderr, "Invalid argument for vhost-tx-cores [0-N]\n");
                goto failed;
            }
            break;

        case CP_SHOW_DASH:
            if (parse_int8(optarg, &c->show_dash) != 0) {
                fprintf(stderr, "Invalid argument for show-dash [0|1]\n");
                goto failed;
            }
            break;

        default:
            fprintf(stderr, "Invalid option\n");
            goto failed;
        }
    }

    return 0;

failed:
    us_vhost_usage(prgname);
    return -1;
}

static int parse_ether_addr(const char *s, struct rte_ether_addr *addr) {
    unsigned int bytes[6];
    int ret;

    // Parse MAC address in format "XX:XX:XX:XX:XX:XX" or "XX-XX-XX-XX-XX-XX"
    ret = sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);

    if (ret != 6) {
        // Try with dashes
        ret = sscanf(s, "%02x-%02x-%02x-%02x-%02x-%02x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
                     &bytes[5]);
    }

    if (ret != 6) {
        fprintf(stderr, "Invalid argument for other-node-mac: %s (expected format: XX:XX:XX:XX:XX:XX)\n", s);
        return -1;
    }

    // Copy parsed bytes to rte_ether_addr structure
    for (int i = 0; i < 6; i++) {
        if (bytes[i] > 0xFF) {
            fprintf(stderr, "Invalid byte value in MAC address: %s\n", s);
            return -1;
        }
        addr->addr_bytes[i] = (uint8_t)bytes[i];
    }

    return 0;
}

static inline int parse_int32(const char *s, uint32_t *pi) {
    char *end;
    *pi = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    return 0;
}

static inline int parse_int16(const char *s, uint16_t *pi) {
    char *end;
    *pi = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    return 0;
}

static inline int parse_int8(const char *s, uint8_t *pi) {
    char *end;
    *pi = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    return 0;
}

static int parse_socket_dir(config_t *c, const char *q_arg) // path e.g. /mnt/huge
{
    // char *old;
    if (access(q_arg, F_OK) == -1) {
        fprintf(stderr, "Invalid argument for socket directory (Directory does not exist)\n");
        return -1;
    }

    /* parse number string */
    if (strnlen(q_arg, PATH_MAX) == PATH_MAX) // check if path is too long
        return -1;

    c->socket_dir = strdup(q_arg);

    return 0;
}

static inline int parse_cidr(char *s, uint32_t *ip, uint8_t *prefix) {
    char *slash;

    /* parse /prefix and replace / by \0 (if applicable)*/
    if ((slash = strrchr(s, '/')) != NULL) {
        if (parse_int8(slash + 1, prefix) != 0) {
            fprintf(stderr, "parse_cidr: parsing prefix (%s) failed\n", slash);
            return -1;
        }
        *slash = 0;
    }

    if (util_parse_ipv4(s, ip) != 0) {
        fprintf(stderr, "parse_cidr: parsing IP (%s) failed\n", s);
        return -1;
    }

    return 0;
}