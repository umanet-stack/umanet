/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <linux/virtio_net.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include <rte_atomic.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_vhost.h>

#include "./include/tas.h"
#include "src/config/config.h"
#include "src/eth/eth.h"
#include "src/tcp_state.h"
#include "src/vhost/vhost.h"
#include "tcp_fastpath.h"

#ifndef MAX_QUEUES
#define MAX_QUEUES 128
#endif

/* the maximum number of external ports supported */
#define MAX_SUP_PORTS 1

#define MBUF_CACHE_SIZE 128
#define MBUF_DATA_SIZE RTE_MBUF_DEFAULT_BUF_SIZE

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024

unsigned fp_cores_max;
volatile unsigned fp_cores_cur = 1;
volatile unsigned fp_scale_to = 0;

struct dataplane_context **ctxs = NULL;

/*
 * This is a thread will wake up after a period to print stats if the user has
 * enabled them.
 */
static void *print_stats(__rte_unused void *arg) {
    struct vhost_dev *vdev;
    uint64_t tx_dropped, rx_dropped;
    uint64_t tx, tx_total, rx, rx_total;
    const char clr[] = {27, '[', '2', 'J', '\0'};
    const char top_left[] = {27, '[', '1', ';', '1', 'H', '\0'};

    while (1) {
        sleep(1);

        /* Clear screen and move to top left */
        printf("%s%s\n", clr, top_left);
        printf("Device statistics =================================\n");

        TAILQ_FOREACH(vdev, &vhost.vhost_dev_list, global_vdev_entry) {
            tx_total = vdev->stats.tx_total;
            tx = vdev->stats.tx;
            tx_dropped = tx_total - tx;

            rx_total = rte_atomic64_read(&vdev->stats.rx_total_atomic);
            rx = rte_atomic64_read(&vdev->stats.rx_atomic);
            rx_dropped = rx_total - rx;

            printf("Statistics for device %d\n"
                   "-----------------------\n"
                   "TX total:              %" PRIu64 "\n"
                   "TX dropped:            %" PRIu64 "\n"
                   "TX successful:         %" PRIu64 "\n"
                   "RX total:              %" PRIu64 "\n"
                   "RX dropped:            %" PRIu64 "\n"
                   "RX successful:         %" PRIu64 "\n",
                   vdev->vid, tx_total, tx_dropped, tx, rx_total, rx_dropped, rx);
        }

        printf("===================================================\n");

        fflush(stdout);
    }

    return NULL;
}

static void unregister_drivers(int socket_num, const char *path) {
    int i, ret;

    for (i = 0; i < socket_num; i++) {
        // each path is PATH_MAX bytes apart
        ret = rte_vhost_driver_unregister(path + i * PATH_MAX);
        if (ret != 0)
            RTE_LOG(ERR, VHOST_CONFIG, "Fail to unregister vhost driver for %s.\n", path + i * PATH_MAX);
    }
}

static int socket_num;
static const char *socket_files;

/* When we receive a INT signal, unregister vhost driver */
static void sigint_handler(__rte_unused int signum) {
    /* Unregister vhost driver. */
    unregister_drivers(socket_num, socket_files);

    exit(0);
}

static unsigned threads_launched = 0;

/*
 * Main function, does initialisation and calls the per-lcore functions.
 */
int main(int argc, char *argv[]) {
    unsigned lcore_id, core_id = 0;
    unsigned nb_ports, valid_num_ports;
    int ret, i;
    uint16_t portid;
    static pthread_t tid;
    uint64_t flags = 0;

    int res = EXIT_SUCCESS;

    // Register signal handler for SIGINT (Ctrl+C) (graceful shutdown)
    signal(SIGINT, sigint_handler);

    /* init EAL (Environment Abstraction Layer) */
    ret = rte_eal_init(argc, argv); // Parses DPDK-specific arguments (--lcores, --huge-dir, etc.)
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    argc -= ret; // Update argc to exclude DPDK-specific arguments
    argv += ret;

    init_config();
    /* parse app arguments */
    ret = parse_config(&config, argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid argument\n");
    socket_num = config.nb_sockets;
    socket_files = config.socket_files;

    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        TAILQ_INIT(&vhost.lcore_info[lcore_id].vdev_list); // init first,last dev list

        if (rte_lcore_is_enabled(lcore_id))
            vhost.lcore_ids[core_id++] = lcore_id;
    }

    if (rte_lcore_count() > RTE_MAX_LCORE)
        rte_exit(EXIT_FAILURE, "Not enough cores\n");

    /* Get the number of physical ports. */
    nb_ports = rte_eth_dev_count_avail(); // Count available (not disabled) Ethernet ports (physical NICs)

    /*
     * Update the global var NUM_PORTS and global array PORTS
     * and get value of var VALID_NUM_PORTS according to system ports number
     */
    valid_num_ports = check_ports_num(nb_ports);

    if ((valid_num_ports == 0) || (valid_num_ports > MAX_SUP_PORTS)) {
        RTE_LOG(INFO, VHOST_PORT,
                "Current enabled port number is %u,"
                "but only %u port can be enabled\n",
                config.num_ports, MAX_SUP_PORTS);
        return -1;
    }

    /*
     * FIXME: here we are trying to allocate mbufs big enough for
     * @MAX_QUEUES, but the truth is we're never going to use that
     * many queues here. We probably should only do allocation for
     * those queues we are going to use.
     */
    // number of worker cores (minus master core)
    create_mbuf_pool(valid_num_ports, rte_lcore_count() - 1, MBUF_DATA_SIZE, MAX_QUEUES, RTE_TEST_RX_DESC_DEFAULT,
                     MBUF_CACHE_SIZE);

    if (config.vm2vm_mode == VM2VM_HARDWARE) {
        /* Enable VT loop back to let L2 switch to do it. */
        config.vmdq_conf_default->rx_adv_conf.vmdq_rx_conf.enable_loop_back = 1;
        RTE_LOG(DEBUG, VHOST_CONFIG, "Enable loop back for L2 switch in vmdq.\n");
    }

    // /* initialize eth port */
    printf("Initializing network ports on cores: ");
    fflush(stdout);
    if (port_init(config.fp_cores_max) != 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize network ports\n");

    // if (network_init(fp_cores_max) != 0) {
    //     res = EXIT_FAILURE;
    //     fprintf(stderr, "network init failed\n");
    //     rte_exit(EXIT_FAILURE, "network init failed\n");
    //     // goto error_shm_cleanup;
    // }

    // NEW: Initialize TCP offload subsystem
    // if (tcp_offload_init(128 * 1024) != 0) {
    //     rte_exit(EXIT_FAILURE, "Cannot initialize TCP offload\n");
    // }
    // RTE_LOG(INFO, VHOST_CONFIG, "TCP offload initialized (pass-through mode)\n");

    /* Enable stats if the user option is set. */
    if (config.enable_stats) {
        ret = rte_ctrl_thread_create(&tid, "print-stats", NULL, print_stats, NULL);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot create print-stats thread\n");
    }

    printf("Launching switch workers on cores: ");
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    rte_eal_remote_launch(switch_worker, NULL, lcore_id);

    // void *arg;
    // /* Launch all data cores. */
    // RTE_LCORE_FOREACH_SLAVE(lcore_id) {
    //     if (threads_launched < fp_cores_max) {
    //         arg = (void *)(uintptr_t)threads_launched;
    //         if (rte_eal_remote_launch(switch_worker, arg, lcore_id) != 0) {
    //             fprintf(stderr, "ERROR\n");
    //             return -1;
    //         }
    //         threads_launched++;
    //     }
    // }

    if (config.client_mode)
        flags |= RTE_VHOST_USER_CLIENT;

    if (config.dequeue_zero_copy)
        flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;

    /* Register vhost user driver to handle vhost messages. */
    for (i = 0; i < config.nb_sockets; i++) {
        char *file = config.socket_files + i * PATH_MAX;
        ret = rte_vhost_driver_register(file, flags);
        if (ret != 0) {
            unregister_drivers(i, socket_files);
            rte_exit(EXIT_FAILURE, "vhost driver register failure.\n");
        }

        if (config.builtin_net_driver)
            rte_vhost_driver_set_features(file, VIRTIO_NET_FEATURES);

        if (config.mergeable == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_MRG_RXBUF);
        }

        if (config.enable_tx_csum == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_CSUM);
        }

        if (config.enable_tso == 0) {
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO4);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_HOST_TSO6);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO4);
            rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_GUEST_TSO6);
        }

        if (config.promiscuous) {
            rte_vhost_driver_enable_features(file, 1ULL << VIRTIO_NET_F_CTRL_RX);
        }

        ret = rte_vhost_driver_callback_register(file, &virtio_net_device_ops);
        if (ret != 0) {
            rte_exit(EXIT_FAILURE, "failed to register vhost driver callbacks.\n");
        }

        if (rte_vhost_driver_start(file) < 0) {
            rte_exit(EXIT_FAILURE, "failed to start vhost driver.\n");
        }
    }

    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    rte_eal_wait_lcore(lcore_id);

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}
