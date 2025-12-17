#include <rte_mbuf.h>

void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}
