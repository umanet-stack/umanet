#ifndef _VHOST_H_
#define _VHOST_H_

#include "main.h"

typedef struct {
    struct lcore_info lcore_info[RTE_MAX_LCORE];
    struct vhost_dev_tailq_list vhost_dev_list;
} vhost_state_t;

extern vhost_state_t vhost;
extern const struct vhost_device_ops virtio_net_device_ops;

void destroy_device(int vid);
int new_device(int vid);
#endif