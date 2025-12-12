/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include "../include/tas.h"
#include "config.h"
#include <getopt.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_memory.h>

#define BURST_RX_WAIT_US 15 /* Defines how long we wait between retries on RX */
#define BURST_RX_RETRIES 4  /* Number of retries on RX. */

static inline int parse_int32(const char *s, uint32_t *pi);
static int parse_num_opt(const char *q_arg, uint32_t max_valid_value);
static int us_vhost_parse_socket_path(config_t *c, const char *q_arg);

config_t config;
void init_config() {
    config = (config_t){
        .enable_stats = 0,
        .enable_retry = 1,
        .burst_rx_delay_time = BURST_RX_WAIT_US,
        .burst_rx_retry_num = BURST_RX_RETRIES,
        .shm_len = 1024 * 1024 * 1024,
        .fp_cores_max = 1,
        .fp_interrupts = 1,
        .fp_xsumoffload = 1,
        .fp_autoscale = 1,
        .fp_hugepages = 1,
        .fp_vlan_strip = 0,
        .fp_poll_interval_tas = 10000,
        .fp_poll_interval_app = 10000,
    };
}

enum cfg_params {
    CP_PORTMASK,
    CP_PROMISCIOUS,
    CP_RX_RETRY,
    CP_RX_RETRY_DELAY,
    CP_RX_RETRY_NUM,
    CP_MERGEABLE,
    CP_STATS,
    CP_SOCKET_FILE,
    CP_TX_CSUM,
    CP_TSO,
    CP_CLIENT,
    CP_DEQUEUE_ZERO_COPY,
    CP_FP_CORES_MAX,
};

static struct option options[] = {
    {
        "portmask",
        required_argument,
        .val = CP_PORTMASK,
    },
    {
        "promiscuous",
        no_argument,
        .val = CP_PROMISCIOUS,
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
        "stats",
        required_argument,
        .val = CP_STATS,
    },
    {
        "socket-file",
        required_argument,
        .val = CP_SOCKET_FILE,
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
    {
        "fp-cores-max",
        required_argument,
        .val = CP_FP_CORES_MAX,
    },
    {"client", no_argument, .val = CP_CLIENT},
    {"dequeue-zero-copy", no_argument, .val = CP_DEQUEUE_ZERO_COPY},
};

/*
 * Display usage
 */
static void us_vhost_usage(const char *prgname) {
    RTE_LOG(INFO, VHOST_CONFIG,
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

/*
 * Parse the arguments given in the command line of the application.
 */
int parse_config(config_t *c, int argc, char **argv) {
    int opt, ret;
    const char *prgname = argv[0];

    while ((opt = getopt_long(argc, argv, "", options, NULL)) != EOF) {
        switch (opt) {
        case CP_PROMISCIOUS:
            c->promiscuous = 1;
            break;

        case CP_RX_RETRY:
            /* Enable/disable retries on RX. */
            ret = parse_num_opt(optarg, 1);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for rx-retry [0|1]\n");
                goto failed;
            } else {
                c->enable_retry = ret;
            }
            break;
        case CP_TX_CSUM:
            /* Enable/disable TX checksum offload. */
            ret = parse_num_opt(optarg, 1);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for tx-csum [0|1]\n");
                goto failed;
            } else
                c->enable_tx_csum = ret;
            break;

        case CP_TSO:
            /* Enable/disable TSO offload. */
            ret = parse_num_opt(optarg, 1);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for tso [0|1]\n");
                goto failed;
            } else
                c->enable_tso = ret;
            break;

        case CP_RX_RETRY_DELAY:
            /* Specify the retries delay time (in useconds) on RX. */
            ret = parse_num_opt(optarg, INT32_MAX);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for rx-retry-delay [0-N]\n");
                goto failed;
            } else {
                c->burst_rx_delay_time = ret;
            }
            break;

        case CP_RX_RETRY_NUM:
            /* Specify the retries number on RX. */
            ret = parse_num_opt(optarg, INT32_MAX);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for rx-retry-num [0-N]\n");
                goto failed;
            } else {
                c->burst_rx_retry_num = ret;
            }
            break;

        case CP_MERGEABLE:
            /* Enable/disable RX mergeable buffers. */
            ret = parse_num_opt(optarg, 1);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for mergeable [0|1]\n");
                goto failed;
            } else {
                c->mergeable = !!ret;
            }
            break;

        case CP_STATS:
            /* Enable/disable stats. */
            ret = parse_num_opt(optarg, INT32_MAX);
            if (ret == -1) {
                fprintf(stderr, "Invalid argument for stats [0..N]\n");
                goto failed;
            } else {
                c->enable_stats = ret;
            }
            break;

        case CP_SOCKET_FILE:
            /* Set socket file path. */
            if (us_vhost_parse_socket_path(c, optarg) == -1) {
                fprintf(stderr, "Invalid argument for socket name (Max %d characters)\n", PATH_MAX);
                goto failed;
            }
            break;

        case CP_FP_CORES_MAX:
            if (parse_int32(optarg, &c->fp_cores_max) != 0) {
                fprintf(stderr, "Invalid argument for fp-cores-max [0-N]\n");
                goto failed;
            }
            break;

        case CP_CLIENT:
            c->client_mode = 1;
            break;
        case CP_DEQUEUE_ZERO_COPY:
            c->dequeue_zero_copy = 1;
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

static inline int parse_int32(const char *s, uint32_t *pi) {
    char *end;
    *pi = strtoul(s, &end, 10);
    if (!*s || *end)
        return -1;
    return 0;
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
 * Set socket file path.
 */
static int us_vhost_parse_socket_path(config_t *c, const char *q_arg) // path e.g. /tmp/vhost-user.sock
{
    char *old;

    /* parse number string */
    if (strnlen(q_arg, PATH_MAX) == PATH_MAX) // check if path is too long
        return -1;

    old = c->socket_files;
    // Reallocates socket_files to fit one more socket path
    c->socket_files = realloc(c->socket_files, PATH_MAX * (c->nb_sockets + 1));
    if (c->socket_files == NULL) { // check if realloc failed
        free(old);
        return -1;
    }

    strlcpy(c->socket_files + c->nb_sockets * PATH_MAX, q_arg, PATH_MAX); // copies path to socket_files' new slot
    c->nb_sockets++;

    return 0;
}