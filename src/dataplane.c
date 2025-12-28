#include "log.h"
#include "src/include/state.h"

int init_dataplane_topology(void) {
    if ((global = calloc(1, sizeof(*global))) == NULL) {
        LOG_ERROR("dataplane_init: failed to allocate global\n");
        return -1;
    }

    return 0;
}
