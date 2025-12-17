#include <rte_mbuf.h>

#include "src/utils/utils.h"

void free_pkts(struct rte_mbuf **pkts, uint16_t n) {
    while (n--)
        rte_pktmbuf_free(pkts[n]);
}
