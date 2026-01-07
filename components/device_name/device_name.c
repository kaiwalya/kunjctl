#include "device_name.h"
#include <stdio.h>
#include "esp_mac.h"

static const char *adjectives[] = {
    "swift", "bright", "calm", "bold", "keen",
    "warm", "cool", "quick", "sharp", "soft",
    "fair", "true", "pure", "wise", "kind",
    "brave", "free", "glad", "proud", "neat",
    "crisp", "fresh", "clear", "prime", "noble",
    "vivid", "stark", "sleek", "spry", "deft"
};

static const char *nouns[] = {
    "falcon", "river", "oak", "fox", "wolf",
    "pine", "hawk", "brook", "stone", "fern",
    "birch", "heron", "cliff", "moss", "reed",
    "wren", "sage", "flint", "grove", "lark",
    "marsh", "peak", "vale", "aspen", "crow",
    "ridge", "spruce", "finch", "dale", "elm"
};

#define NUM_ADJ   (sizeof(adjectives) / sizeof(adjectives[0]))
#define NUM_NOUNS (sizeof(nouns) / sizeof(nouns[0]))

void device_name_get(char *buf, size_t len) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    /* Use last 4 bytes of MAC for seed and suffix */
    uint32_t seed = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    uint16_t suffix = (mac[4] << 8) | mac[5];

    const char *adj = adjectives[seed % NUM_ADJ];
    const char *noun = nouns[(seed / NUM_ADJ) % NUM_NOUNS];

    snprintf(buf, len, "%s-%s-%04x", adj, noun, suffix);
}
