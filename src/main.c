/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <linux/virtio_net.h>
#include <pthread.h>
#include <rte_malloc.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <rte_atomic.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_vhost.h>

#include "./config/config.h"
#include "./include/main.h"
#include "log.h"
#include "src/include/state.h"
#include "src/network/network.h"
#include "src/slow/slowpath.h"
#include "src/vhost/vhost.h"
#include <stdatomic.h>

config_t config;

struct dataplane_topology *global = NULL;
struct eth_tx_ctx **eth_tx_ctxs = NULL;
struct eth_rx_ctx **eth_rx_ctxs = NULL;
struct vhost_tx_ctx **vhost_tx_ctxs = NULL;
struct vhost_rx_ctx **vhost_rx_ctxs = NULL;
struct control_ctx *control_ctx = NULL;
_Atomic(struct vdev_list *) vdev_list = NULL;

static int start_threads(void);
static void thread_error(void);
static int common_thread(void *arg);

static void sigint_handler(__rte_unused int signum) {
    unregister_vhost_drivers(config.nb_sockets, config.socket_files);
    // dataplane_dump_stats();
    // network_dump_stats(); // Dump hardware TX/RX statistics including errors
    exit(0);
}

int main(int argc, char *argv[]) {
    int res = EXIT_SUCCESS;

    // Register signal handler for SIGINT (Ctrl+C) (graceful shutdown)
    signal(SIGINT, sigint_handler);

    /* init DPDK EAL (Environment Abstraction Layer) */
    rte_log_set_global_level(RTE_LOG_ERR);
    int dpdk_args = rte_eal_init(argc, argv); // Parses DPDK-specific arguments (--lcores, --huge-dir, etc.)
    if (dpdk_args < 0) {
        LOG_ERROR("dpdk init failed\n");
        res = EXIT_FAILURE;
        goto error_exit;
    }
    LOG_IMPT("✅ Initialized DPDK EAL\n");
    argc -= dpdk_args; // Update argc to exclude DPDK-specific arguments
    argv += dpdk_args;

    init_config(&config);
    if (parse_config(&config, argc, argv) != 0) {
        LOG_ERROR("invalid argument\n");
        res = EXIT_FAILURE;
        goto error_exit;
    }
    LOG_IMPT("✅ Parsed config\n");

    if (init_dataplane_topology() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("init_dataplane_topology failed\n");
        goto error_exit;
    }
    LOG_IMPT("✅ Initialized dataplane topology\n");

    if (init_rings() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("init_rings failed\n");
        goto error_exit;
    }
    LOG_IMPT("✅ Initialized rings\n");

    if (init_dataplane_ctxs() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("init_dataplane_ctxs failed\n");
        goto error_exit;
    }
    LOG_IMPT("✅ Initialized dataplane contexts\n");

    if (init_vhost_plans() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("init_vhost_rx_plans failed\n");
        goto error_exit;
    }
    LOG_IMPT("✅ Initialized vhost RX plans\n");

    // Sets up RX/TX queues per core
    if (network_init() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("network init failed\n");
        goto error_network_cleanup;
    }
    LOG_IMPT("✅ Initialized network\n");

    // if (init_mac_flow_table() != 0) {
    //     res = EXIT_FAILURE;
    //     LOG_ERROR("init_mac_flow_table failed\n");
    //     goto error_network_cleanup;
    // }
    // LOG_IMPT("✅ Initialized mac flow table\n");

    // Start worker threads BEFORE vhost registration
    // This ensures TX queues are initialized before vhost can send packets
    if (start_threads() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("start_threads failed\n");
        goto error_dataplane_cleanup;
    }

    LOG_INFO("Waiting for worker threads to initialize TX/RX queues...\n");
    int max_wait = 10; // 10 seconds max
    int all_ready = 0;
    for (int wait = 0; wait < max_wait && !all_ready; wait++) {
        sleep(1);
        all_ready = 1;
        for (int i = 0; i < config.eth_rx_cores; i++) {
            if (eth_rx_ctxs[i] == NULL) {
                all_ready = 0;
                break;
            }
        }
        for (int i = 0; i < config.eth_tx_cores; i++) {
            if (eth_tx_ctxs[i] == NULL) {
                all_ready = 0;
                break;
            }
        }
        for (int i = 0; i < config.vhost_rx_cores; i++) {
            if (vhost_rx_ctxs[i] == NULL) {
                all_ready = 0;
                break;
            }
        }
        for (int i = 0; i < config.vhost_tx_cores; i++) {
            if (vhost_tx_ctxs[i] == NULL) {
                all_ready = 0;
                break;
            }
        }
    }

    if (network_start_eth() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("network_start_eth failed\n");
        goto error_dataplane_cleanup;
    }
    LOG_IMPT("✅ Started network\n");

    if (!all_ready) {
        res = EXIT_FAILURE;
        LOG_ERROR("ERROR: Not all dataplane contexts initialized after %d seconds\n", max_wait);
        goto error_dataplane_cleanup;
    }

    LOG_IMPT("✅ All %d dataplane contexts initialized successfully\n", global->fp_cores);

    if (register_vhost_drivers() != 0) {
        res = EXIT_FAILURE;
        LOG_ERROR("register_vhost_drivers failed\n");
        goto error_dataplane_cleanup;
    }

    slowpath_loop(control_ctx);

    // Wait for lcores to finish (keeps main alive)
    // unsigned lcore_id;
    // RTE_LCORE_FOREACH_WORKER(lcore_id) { rte_eal_wait_lcore(lcore_id); }

    rte_eal_cleanup();

    return 0;

error_dataplane_cleanup:
error_network_cleanup:
    network_cleanup();
error_exit:
    return res;
}

static int common_thread(void *arg) {
    uint16_t id = (uintptr_t)arg;

    {
        char name[17];
        snprintf(name, sizeof(name), "fp-core-%u", id);
        pthread_setname_np(pthread_self(), name);
    }

    // id starts at 1, but arrays are 0-indexed, so subtract 1
    if (id <= config.eth_rx_cores) {
        struct eth_rx_ctx *eth_rx_ctx = eth_rx_ctxs[id - 1];
        eth_rx_ctx->core_id = id;
        if (network_rx_queue_init(eth_rx_ctx) != 0) {
            LOG_ERROR("network_rx_queue_init failed\n");
            return -1;
        }
        eth_rx_loop(eth_rx_ctx);

    } else if (id <= config.eth_rx_cores + config.eth_tx_cores) {
        struct eth_tx_ctx *eth_tx_ctx = eth_tx_ctxs[id - config.eth_rx_cores - 1];
        eth_tx_ctx->core_id = id;
        if (network_tx_queue_init(eth_tx_ctx) != 0) {
            LOG_ERROR("network_tx_queue_init failed\n");
            return -1;
        }
        eth_tx_loop(eth_tx_ctx);

    } else if (id <= config.eth_rx_cores + config.eth_tx_cores + config.vhost_rx_cores) {
        struct vhost_rx_ctx *vhost_rx_ctx = vhost_rx_ctxs[id - config.eth_rx_cores - config.eth_tx_cores - 1];
        vhost_rx_ctx->core_id = id;
        vhost_rx_loop(vhost_rx_ctx);

    } else if (id <= config.eth_rx_cores + config.eth_tx_cores + config.vhost_rx_cores + config.vhost_tx_cores) {
        struct vhost_tx_ctx *vhost_tx_ctx =
            vhost_tx_ctxs[id - config.eth_rx_cores - config.eth_tx_cores - config.vhost_rx_cores - 1];
        vhost_tx_ctx->core_id = id;
        vhost_tx_loop(vhost_tx_ctx);

    } else {
        LOG_ERROR("Invalid core ID: %u\n", id);
        thread_error();
        return -1;
    }

    return 0;
}

static int start_threads(void) {
    unsigned cores_avail, cores_needed, core;
    void *arg;

    cores_avail = rte_lcore_count();
    // 8 fast path cores + 1 slow path core
    // -l 0-8 = 1 master core (core 0) + 8 slave cores (core 1-8, tho id will be 0-7)
    cores_needed = FP_CORES + 1;

    if (cores_avail < cores_needed) {
        LOG_ERROR("Not enough cores: got %u, need %u\n", cores_avail, cores_needed);
        return -1;
    }

    uint16_t threads_launched = 0;
    RTE_LCORE_FOREACH_WORKER(core) {
        if (threads_launched < global->fp_cores) {
            arg = (void *)(uintptr_t)(threads_launched + 1);
            if (rte_eal_remote_launch(common_thread, arg, core) != 0) {
                LOG_ERROR("ERROR\n");
                return -1;
            }
            threads_launched++;
        }
    }

    return 0;
}

static void thread_error(void) {
    LOG_ERROR("thread_error\n");
    abort();
}
