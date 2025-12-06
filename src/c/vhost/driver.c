#include <rte_vhost.h>

int register_driver(const char *socket_path, const int client_mode, const int dequeue_zero_copy) {
    uint64_t flags = 0;
    if (client_mode)
        flags |= RTE_VHOST_USER_CLIENT;
    if (dequeue_zero_copy)
        flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;

    return rte_vhost_driver_register(socket_path, flags);
}

int unregister_driver(const char *socket_path) { return rte_vhost_driver_unregister(socket_path); }

int set_features(const char *socket_path, const uint64_t features) {
    return rte_vhost_driver_set_features(socket_path, features);
}

int disable_features(const char *socket_path, const uint64_t features) {
    return rte_vhost_driver_disable_features(socket_path, features);
}

int enable_features(const char *socket_path, const uint64_t features) {
    return rte_vhost_driver_enable_features(socket_path, features);
}