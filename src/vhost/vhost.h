#ifndef _VHOST_H_
#define _VHOST_H_

#include "main.h"

/* State of virtio device. */
#define DEVICE_MAC_LEARNING 0
#define DEVICE_RX 1
#define DEVICE_SAFE_REMOVE 2

typedef struct {
    struct lcore_info lcore_info[RTE_MAX_LCORE];
    struct vhost_dev_tailq_list vhost_dev_list;
} vhost_state_t;

extern vhost_state_t vhost;
extern const struct vhost_device_ops virtio_net_device_ops;

struct vhost_dev *find_vhost_dev(struct rte_ether_addr *mac);

#endif