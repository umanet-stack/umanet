/* veth interface connection for forwarding packets to host network stack */

#ifndef TAP_H_
#define TAP_H_

#include <rte_mbuf.h>

/**
 * Connect to existing veth interface (vtap0)
 * vtap0 must be created first by setup_vtap.sh
 * Returns 0 on success, -1 on failure
 */
int tap_init(void);

/**
 * Disconnect from veth interface
 */
void tap_cleanup(void);

/**
 * Forward a single packet to veth interface
 * The packet mbuf is freed after transmission
 * Returns 0 on success, -1 on failure
 */
int tap_tx_one(struct rte_mbuf *pkt);

/**
 * Forward multiple packets to veth interface
 * All packet mbufs are freed after transmission
 * Returns number of packets successfully sent
 */
int tap_tx_burst(struct rte_mbuf **pkts, uint16_t count);

#endif /* TAP_H_ */
