#pragma once

#include <stdbool.h>

typedef struct relay_t relay_t;

/*
 * Initialize the relay and bring it to `initial_state`.
 *
 * `force_resync` controls how the relay is brought into sync with
 * `initial_state` for latching relays, where the mechanical state is not
 * readable and may disagree with the persisted software state after a power
 * loss. Pass true on cold boot to pulse the appropriate coil unconditionally;
 * pass false on a deep-sleep wake to trust the persisted state and avoid
 * unnecessary coil actuations. Has no effect for level-driven relays.
 */
relay_t *relay_init(bool initial_state, bool force_resync);
void relay_deinit(relay_t *relay);

void relay_set(relay_t *relay, bool on);

/* Returns NULL if relay not configured */
const bool *relay_get_state(relay_t *relay);
