#ifndef MAIN_H_
#define MAIN_H_

#include "../config/config.h"
#include "src/include/state.h"

extern config_t config;

int init_dataplane_topology();
int init_dataplane_ctxs();
int init_rings();
void destroy_rings();

void eth_rx_loop(struct eth_rx_ctx *ctx);
void eth_tx_loop(struct eth_tx_ctx *ctx);
void vhost_rx_loop(struct vhost_rx_ctx *ctx);
void vhost_tx_loop(struct vhost_tx_ctx *ctx);
void fp_loop(struct fp_ctx *ctx);

#endif /* MAIN_H_ */
