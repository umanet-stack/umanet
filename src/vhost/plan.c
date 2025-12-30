
#include "log.h"
#include "src/include/state.h"
#include <rte_malloc.h>
#include <stdatomic.h>

_Atomic(struct vhost_rx_plan *) *vhost_rx_plans = NULL;

int init_vhost_rx_plans() {
    vhost_rx_plans = rte_calloc("vhost_rx_plans", global->vhost_rx_cores, sizeof(_Atomic(struct vhost_rx_plan *)), 0);
    if (vhost_rx_plans == NULL) {
        LOG_ERROR("Failed to allocate memory for vhost_rx_plans\n");
        return -1;
    }
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_plan *plan = rte_zmalloc("vhost_rx_plan", sizeof(struct vhost_rx_plan), RTE_CACHE_LINE_SIZE);
        if (plan == NULL) {
            LOG_ERROR("Failed to allocate memory for vhost_rx_plan[%d]\n", i);
            return -1;
        }
        plan->num = 0;
        atomic_store_explicit(&vhost_rx_plans[i], plan, memory_order_release);
    }

    return 0;
}

int vhost_rx_plan_add(int vid) {
    int min_vhost_rx_core_id = -1;
    uint16_t min_vdev = MAX_VHOSTS;
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_plan *plan = atomic_load_explicit(&vhost_rx_plans[i], memory_order_acquire);
        if (plan->num < min_vdev) {
            min_vdev = plan->num;
            min_vhost_rx_core_id = i;
        }
    }

    if (min_vhost_rx_core_id == -1) {
        LOG_ERROR("Failed to find a suitable vhost_rx_plan\n");
        return -1;
    }

    struct vhost_rx_plan *old, *new;
    while (1) {
        old = atomic_load_explicit(&vhost_rx_plans[min_vhost_rx_core_id], memory_order_acquire);

        if (old->num >= MAX_VHOSTS) {
            LOG_ERROR("vhost_rx_plan full\n");
            return -1;
        }

        new = rte_malloc(NULL, sizeof(*new), RTE_CACHE_LINE_SIZE);
        if (!new) {
            LOG_ERROR("allocation failed\n");
            return -1;
        }
        memcpy(new, old, sizeof(*new));

        new->vids[new->num] = vid;
        new->num++;

        if (atomic_compare_exchange_weak_explicit(&vhost_rx_plans[min_vhost_rx_core_id], &old, new,
                                                  memory_order_release, memory_order_acquire)) {
            break; // success
        }

        // CAS failed — someone updated concurrently
        rte_free(new);
    }

    return 0;
}

int vhost_rx_plan_remove(int vid) {
    int vhost_rx_core_id = -1;
    for (int i = 0; i < global->vhost_rx_cores; i++) {
        struct vhost_rx_plan *plan = atomic_load_explicit(&vhost_rx_plans[i], memory_order_acquire);
        for (int j = 0; j < plan->num; j++) {
            if (plan->vids[j] == vid) {
                vhost_rx_core_id = i;
            }
        }
        if (vhost_rx_core_id != -1)
            break;
    }

    if (vhost_rx_core_id == -1) {
        LOG_ERROR("Failed to find vhost_rx_core_id for vid=%d\n", vid);
        return -1;
    }

    struct vhost_rx_plan *old, *new;
    while (1) {
        old = atomic_load_explicit(&vhost_rx_plans[vhost_rx_core_id], memory_order_acquire);

        new = rte_malloc(NULL, sizeof(*new), RTE_CACHE_LINE_SIZE);
        if (!new) {
            LOG_ERROR("allocation failed\n");
            return -1;
        }
        memcpy(new, old, sizeof(*new));

        int idx = 0;
        for (int i = 0; i < old->num; i++) {
            if (new->vids[i] == vid) {
                idx = i;
                break;
            }
        }

        for (int i = idx; i < old->num - 1; i++) {
            new->vids[i] = old->vids[i + 1];
        }
        new->num = old->num - 1;

        if (atomic_compare_exchange_weak_explicit(&vhost_rx_plans[vhost_rx_core_id], &old, new, memory_order_release,
                                                  memory_order_acquire)) {
            break; // success
        }

        // CAS failed — someone updated concurrently
        rte_free(new);
    }

    return 0;
}