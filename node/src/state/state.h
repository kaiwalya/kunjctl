#pragma once

typedef struct state_t state_t;

typedef enum {
    PAIRING_STATE_UNPAIRED = 0,
    PAIRING_STATE_PAIRED = 1,
} pairing_state_t;

state_t *state_init(void);
void state_deinit(state_t *state);

pairing_state_t state_get_pairing(state_t *state);
void state_set_pairing(state_t *state, pairing_state_t pairing);
