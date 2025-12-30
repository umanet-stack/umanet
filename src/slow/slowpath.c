#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <unistd.h>

void slowpath_loop(void) {
    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif
    }
}
