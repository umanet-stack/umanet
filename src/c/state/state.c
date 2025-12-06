#include "state.h"
#include <stdlib.h>
#include <string.h>

void init_state(AppState *app_state) {
    app_state = malloc(sizeof(AppState));
    memset(app_state, 0, sizeof(AppState));
}
