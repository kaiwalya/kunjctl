#include "state.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "state";
#define NVS_NAMESPACE "state"
#define NVS_KEY_PAIRING "pairing"

struct state_t {
    nvs_handle_t nvs;
    pairing_state_t pairing;
};

/*── Repository (NVS abstraction) ──*/

static pairing_state_t repo_load_pairing(nvs_handle_t nvs) {
    uint8_t val = PAIRING_STATE_UNPAIRED;
    nvs_get_u8(nvs, NVS_KEY_PAIRING, &val);
    return (pairing_state_t)val;
}

static void repo_save_pairing(nvs_handle_t nvs, pairing_state_t pairing) {
    nvs_set_u8(nvs, NVS_KEY_PAIRING, (uint8_t)pairing);
    nvs_commit(nvs);
}

/*── Domain (Public API) ──*/

state_t *state_init(void) {
    state_t *s = malloc(sizeof(state_t));
    if (!s) return NULL;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s->nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        free(s);
        return NULL;
    }

    s->pairing = repo_load_pairing(s->nvs);
    ESP_LOGI(TAG, "State initialized (pairing=%d)", s->pairing);
    return s;
}

void state_deinit(state_t *state) {
    if (!state) return;
    nvs_close(state->nvs);
    free(state);
}

pairing_state_t state_get_pairing(state_t *state) {
    return state->pairing;
}

void state_set_pairing(state_t *state, pairing_state_t pairing) {
    if (state->pairing == pairing) return;
    state->pairing = pairing;
    repo_save_pairing(state->nvs, pairing);
    ESP_LOGI(TAG, "Pairing state changed to %d", pairing);
}
