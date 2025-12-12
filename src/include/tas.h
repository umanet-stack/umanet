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

#ifndef TAS_H_
#define TAS_H_

#include "../../include/packet_defs.h"
#include "../../include/tas_memif.h"
#include "../config/config.h"

/** @addtogroup tas
 *  @brief TAS.
 */

extern config_t config;

extern void *tas_shm;
extern struct flextcp_pl_mem *fp_state;
// extern struct flexnic_info *tas_info;
#if RTE_VER_YEAR < 19
extern struct ether_addr eth_addr;
#else
extern struct rte_ether_addr eth_addr;
#endif
extern unsigned fp_cores_max;

int slowpath_main(void);

int shm_preinit(void);
int shm_init(unsigned num);
void shm_cleanup(void);
void shm_set_ready(void);

int network_init(unsigned num_threads);
void network_cleanup(void);

/* used by trace and shm */
void *util_create_shmsiszed(const char *name, size_t size, void *addr);

struct notify_blockstate {
    uint64_t last_active_ts;
    int can_block;
    int second_bar;
};

void notify_fastpath_core(unsigned core);
// void notify_appctx(struct flextcp_pl_appctx *ctx, uint64_t tsc);
void notify_app_core(int appfd, uint64_t *last_tsc);
void notify_slowpath_core(void);
int notify_canblock(struct notify_blockstate *nbs, int had_data, uint64_t tsc);
void notify_canblock_reset(struct notify_blockstate *nbs);

/* should become config options */
#define FLEXNIC_INTERNAL_MEM_SIZE (1024 * 1024 * 32)
#define FLEXNIC_NUM_QMQUEUES (128 * 1024)

#include <rte_vhost.h>
#include <sys/queue.h>

// rte = runtime env (dpdk)
#include <rte_ether.h>

/* Macros for printing using RTE_LOG */
// vhost ops log types: config/data/port
#define RTE_LOGTYPE_VHOST_CONFIG RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_VHOST_DATA RTE_LOGTYPE_USER2
#define RTE_LOGTYPE_VHOST_PORT RTE_LOGTYPE_USER3

// queue type identifiers: receive, transmit, total count
enum { VIRTIO_RXQ, VIRTIO_TXQ, VIRTIO_QNUM };

#define MAX_PKT_BURST 32 /* Max packets processed per burst (RX/TX) */

struct device_statistics {
    uint64_t tx;
    uint64_t tx_total;
    rte_atomic64_t rx_atomic;
    rte_atomic64_t rx_total_atomic;
};

// https://www.redhat.com/en/blog/journey-vhost-users-realm
// https://www.redhat.com/en/blog/virtqueues-and-virtio-ring-how-data-travels
struct vhost_queue {
    struct rte_vhost_vring vr; // DPDK vhost vring
    uint16_t last_avail_idx;   // last processed available descriptor index
    uint16_t last_used_idx;    // last processed used descriptor index
};

struct vhost_dev { // vhost device
    /**< Number of memory regions for gpa to hpa translation. */
    uint32_t nregions_hpa;
    /**< Device MAC address (Obtained on first TX packet). */
    struct rte_ether_addr mac_address;
    /**< RX VMDQ (VM device queue) queue number. */
    uint16_t vmdq_rx_q; // stores the RX queue number assigned to each vhost device
    /**< Vlan tag assigned to the pool */
    uint32_t vlan_tag;
    /**< Data core that the device is added to. */
    uint16_t coreid;
    /**< A device is set as ready if the MAC address has been set. */
    volatile uint8_t ready;
    /**< Device is marked for removal from the data core. */
    volatile uint8_t remove;

    int vid;                      // vhost device ID
    uint64_t features;            // Virtio feature flags
    size_t hdr_len;               // Header length
    uint16_t nr_vrings;           // Number of virtio rings
    struct rte_vhost_memory *mem; // Guest memory mapping
    struct device_statistics stats;
    TAILQ_ENTRY(vhost_dev) global_vdev_entry; // Global list entry
    TAILQ_ENTRY(vhost_dev) lcore_vdev_entry;  // Per-core list entry

#define MAX_QUEUE_PAIRS 4
    struct vhost_queue queues[MAX_QUEUE_PAIRS * 2]; // 4 pairs of RX/TX queues

    // NEW: TCP offload support
    int tcp_offload_enabled;
    struct tcp_flow_state *flows; // Flows associated with this VM
} __rte_cache_aligned;

TAILQ_HEAD(vhost_dev_tailq_list, vhost_dev);

#define REQUEST_DEV_REMOVAL 1
#define ACK_DEV_REMOVAL 0

/*
 * Structure containing data core specific information.
 */
struct lcore_info {
    uint32_t device_num;

    /* Flag to synchronize device removal. */
    volatile uint8_t dev_removal_flag;

    // list of devices on this core
    struct vhost_dev_tailq_list vdev_list;
};

/* we implement non-extra virtio net features (0 = no extra features) */
#define VIRTIO_NET_FEATURES 0

#endif /* ndef TAS_H_ */
