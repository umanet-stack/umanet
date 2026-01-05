#include "src/include/fastpath.h"
#include <rte_cycles.h>
#include <stdint.h>
enum rx_state {
    RX_HOT,   // poll every iteration
    RX_WARM,  // poll 1/2
    RX_COOL,  // poll 1/4
    RX_COLD,  // poll 1/8
    RX_FROZEN // time-based sleep
};

#define RX_BACKOFF 1000000 // 1ms
#define LOW_PKT_BURST MAX_PKT_BURST / 4

// adaptive polling for vms
struct vhost_ap {
    enum rx_state state;
    uint32_t low_polls;
    uint64_t blocked_until_tsc;
};

static inline void update_rx_state(struct vhost_ap *vhost_ap, int poll_num, uint32_t *poll_states) {
    if (poll_num == MAX_PKT_BURST) {
        vhost_ap->state = RX_HOT;
        vhost_ap->low_polls = 0;
        poll_states[0]++;
        return;
    }
    if (poll_num < LOW_PKT_BURST) {
        vhost_ap->low_polls++;
        if (vhost_ap->low_polls == 8) {
            vhost_ap->state = RX_WARM;
            poll_states[1]++;
        } else if (vhost_ap->low_polls == 16) {
            vhost_ap->state = RX_COOL;
            poll_states[2]++;
        } else if (vhost_ap->low_polls == 32) {
            vhost_ap->state = RX_COLD;
            poll_states[3]++;
        } else if (vhost_ap->low_polls == 64) {
            vhost_ap->state = RX_FROZEN;
            vhost_ap->blocked_until_tsc = rte_rdtsc() + RX_BACKOFF;
            poll_states[4]++;
        }
    }
}