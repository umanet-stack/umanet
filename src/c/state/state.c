#include "state.h"
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

// most config -> hardcode, add field-by-field when necessary from zig
void init_state(AppState *app_state) {
    app_state = malloc(sizeof(AppState));
    memset(app_state, 0, sizeof(AppState));

    app_state->enabled_port_mask = 0;
    app_state->promiscuous = 0;
    app_state->num_queues = 0;
    app_state->num_devices = 0;
    app_state->mbuf_pool = NULL;
    app_state->mergeable = 0;
    app_state->vm2vm_mode = VM2VM_SOFTWARE;
    app_state->enable_stats = 0;
    app_state->enable_retry = 1;
    app_state->enable_tx_csum = 0;
    app_state->enable_tso = 0;
    app_state->client_mode = 0;
    app_state->dequeue_zero_copy = 0;
    app_state->builtin_net_driver = 0;
    app_state->burst_rx_delay_time = BURST_RX_WAIT_US;
    app_state->burst_rx_retry_num = BURST_RX_RETRIES;
    app_state->socket_files = NULL;
    app_state->nb_sockets = 0;
    // app_state->lcore_ids = (unsigned *)malloc(sizeof(unsigned) * RTE_MAX_LCORE);
    // app_state->ports = (uint16_t *)malloc(sizeof(uint16_t) * RTE_MAX_ETHPORTS);
    app_state->num_ports = 0;
    app_state->num_pf_queues = 0;
    app_state->num_vmdq_queues = 0;
    app_state->vmdq_pool_base = 0;
    app_state->vmdq_queue_base = 0;
    app_state->queues_per_pool = 0;
    app_state->vmdq_enabled = 0;

    TAILQ_INIT(&app_state->vhost_dev_list);
}
