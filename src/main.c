/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <linux/virtio_net.h>
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
#include "./include/tas.h"
#include "src/include/fastpath.h"
#include "src/vhost/vhost.h"

struct core_load {
    uint64_t cyc_busy;
};

unsigned fp_cores_max;
volatile unsigned fp_cores_cur = 1;
volatile unsigned fp_scale_to = 0;

struct dataplane_context **ctxs = NULL;
struct core_load *core_loads = NULL;

static int start_threads(void);
static void thread_error(void);
static int common_thread(void *arg);

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

static int socket_num;
static const char *socket_files;

static void sigint_handler(__rte_unused int signum) {
    unregister_vhost_drivers(socket_num, socket_files);
    exit(0);
}

static unsigned threads_launched = 0;

int main(int argc, char *argv[]) {
    int res = EXIT_SUCCESS;
    unsigned lcore_id, core_id = 0;

    // Register signal handler for SIGINT (Ctrl+C) (graceful shutdown)
    signal(SIGINT, sigint_handler);

    /* initialize config with defaults before using it */
    init_config();

    /* allocate shared memory before dpdk grabs all huge pages */
    if (shm_preinit() != 0) {
        fprintf(stderr, "shm preinit failed\n");
        res = EXIT_FAILURE;
        goto error_exit;
    }

    /* init DPDK EAL (Environment Abstraction Layer) */
    rte_log_set_global_level(RTE_LOG_ERR);
    int dpdk_args = rte_eal_init(argc, argv); // Parses DPDK-specific arguments (--lcores, --huge-dir, etc.)
    if (dpdk_args < 0) {
        fprintf(stderr, "dpdk init failed\n");
        res = EXIT_FAILURE;
        goto error_exit;
    }
    argc -= dpdk_args; // Update argc to exclude DPDK-specific arguments
    argv += dpdk_args;

    /* parse app arguments */
    if (parse_config(&config, argc, argv) != 0) {
        fprintf(stderr, "invalid argument\n");
        res = EXIT_FAILURE;
        goto error_exit;
    }
    socket_num = config.nb_sockets;
    socket_files = config.socket_files;
    fp_cores_max = config.fp_cores_max;

    if ((core_loads = calloc(fp_cores_max, sizeof(*core_loads))) == NULL) {
        res = EXIT_FAILURE;
        fprintf(stderr, "core loads alloc failed\n");
        goto error_exit;
    }

    // Sets up application queues and DMA regions
    if (shm_init(fp_cores_max) != 0) {
        res = EXIT_FAILURE;
        fprintf(stderr, "dma init failed\n");
        goto error_exit;
    }

    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        TAILQ_INIT(&vhost.lcore_info[lcore_id].vdev_list); // init first,last dev list

        if (rte_lcore_is_enabled(lcore_id))
            vhost.lcore_ids[core_id++] = lcore_id;
    }

    // Sets up RX/TX queues per core, initializes ARP, routing tables
    printf("Initializing network...\n");
    if (network_init(fp_cores_max) != 0) {
        res = EXIT_FAILURE;
        fprintf(stderr, "network init failed\n");
        goto error_shm_cleanup;
    }

    printf("Checking dataplane config...\n");
    if (dataplane_init() != 0) {
        res = EXIT_FAILURE;
        fprintf(stderr, "dpinit failed\n");
        goto error_network_cleanup;
    }

    // Sets flag in shared memory indicating TAS is ready, app waiting to connect can now proceed
    printf("Marking shm ready...\n");
    shm_set_ready();

    /* Enable stats if the user option is set. */
    static pthread_t tid;
    if (config.enable_stats && rte_ctrl_thread_create(&tid, "print-stats", NULL, print_stats, NULL) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot create print-stats thread\n");
    }

    // Start worker threads BEFORE vhost registration
    // This ensures TX queues are initialized before vhost can send packets
    printf("Launching switch workers on cores: ");
    if (start_threads() != 0) {
        res = EXIT_FAILURE;
        fprintf(stderr, "start_threads failed\n");
        goto error_dataplane_cleanup;
    }

    printf("Waiting for worker threads to initialize TX/RX queues...\n");
    sleep(1);
    if (register_vhost_drivers() != 0) {
        res = EXIT_FAILURE;
        fprintf(stderr, "register_vhost_drivers failed\n");
        goto error_dataplane_cleanup;
    }

    // Wait for lcores to finish (keeps main alive)
    RTE_LCORE_FOREACH_SLAVE(lcore_id) { rte_eal_wait_lcore(lcore_id); }

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;

error_dataplane_cleanup:
error_network_cleanup:
    network_cleanup();
error_shm_cleanup:
    shm_cleanup();
error_exit:
    return res;
}

static int common_thread(void *arg) {
    uint16_t id = (uintptr_t)arg;
    struct dataplane_context *ctx;

    {
        char name[17];
        snprintf(name, sizeof(name), "stcp-fp-%u", id);
        pthread_setname_np(pthread_self(), name);
    }

    /* Allocate fastpath core context */
    if ((ctx = rte_zmalloc("fastpath core context", sizeof(*ctx), 0)) == NULL) {
        fprintf(stderr, "Allocating fastpath core context failed\n");
        goto error_alloc;
    }
    ctxs[id] = ctx;
    ctx->id = id;

    /* initialize trace if enabled */
#ifdef FLEXNIC_TRACING
    if (trace_thread_init(id) != 0) {
        fprintf(stderr, "initializing trace failed\n");
        goto error_trace;
    }
#endif

    /* initialize data plane context */
    if (dataplane_context_init(ctx) != 0) {
        fprintf(stderr, "initializing data plane context\n");
        goto error_dpctx;
    }

    /* poll doorbells and network */
    printf("Entering dataplane loop...\n");
    dataplane_loop(ctx);

    dataplane_context_destroy(ctx);
    return 0;

error_dpctx:
#ifdef FLEXNIC_TRACING
error_trace:
#endif
    dataplane_context_destroy(ctx);
error_alloc:
    thread_error();
    return -1;
}

static int start_threads(void) {
    unsigned cores_avail, cores_needed, core;
    void *arg;

    cores_avail = rte_lcore_count();
    /* fast path cores + one slow path core */
    cores_needed = fp_cores_max + 1;

    if ((ctxs = rte_calloc("context list", fp_cores_max, sizeof(*ctxs), 64)) == NULL) {
        perror("datplane_init: calloc failed");
        return -1;
    }

    /* check that we have enough cores */
    if (cores_avail < cores_needed) {
        fprintf(stderr, "Not enough cores: got %u need %u\n", cores_avail, cores_needed);
        return -1;
    }

    /* start common threads */
    RTE_LCORE_FOREACH_SLAVE(core) {
        if (threads_launched < fp_cores_max) {
            arg = (void *)(uintptr_t)threads_launched;
            if (rte_eal_remote_launch(common_thread, arg, core) != 0) {
                fprintf(stderr, "ERROR\n");
                return -1;
            }
            threads_launched++;
        }
    }

    return 0;
}

static void thread_error(void) {
    fprintf(stderr, "thread_error\n");
    abort();
}

// int flexnic_scale_to(uint32_t cores) {
//     if (fp_scale_to != 0) {
//         fprintf(stderr, "flexnic_scale_to: already scaling\n");
//         return -1;
//     }

//     fp_scale_to = cores;

//     notify_fastpath_core(0);
//     return 0;
// }
