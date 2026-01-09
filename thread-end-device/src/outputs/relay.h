#pragma once

#include <stdbool.h>

typedef struct relay_t relay_t;

relay_t *relay_init(bool initial_state);
void relay_deinit(relay_t *relay);

void relay_set(relay_t *relay, bool on);

/* Returns NULL if relay not configured */
const bool *relay_get_state(relay_t *relay);
