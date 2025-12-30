#include "src/include/fastpath.h"
#include "src/include/state.h"
#include <unistd.h>

void slowpath_loop(void) {
    LOG_IMPT("Entering slowpath loop...\n");

    while (1) {
        STATS_TS(start);
#ifdef DEBUG
        sleep(1);
#endif

        struct vdev_list *vdevs = atomic_load(&vdev_list);
        for (int i = 0; i < vdevs->num; i++) {
        }
    }
}
