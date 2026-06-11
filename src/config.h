#ifndef MIRROR_CONFIG_H
#define MIRROR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Layer configuration, read from environment variables:
 *
 *   MIRROR_MODE      - "depth" (default) or "off".
 *   MIRROR_HIGHLIGHT - comma-separated list of <hash>=<RRGGBB> entries, where
 *                      <hash> is the 64-bit FNV-1a hash (hex) of a fragment
 *                      shader's SPIR-V and <RRGGBB> is an sRGB hex color.
 *                      Example: "f0e1d2c3b4a59687=00ff00,123456789abcdef0=ffcc00"
 *   MIRROR_LOG       - any non-empty value except "0" enables stderr logging.
 */

enum mirror_mode {
    MIRROR_MODE_OFF,
    MIRROR_MODE_DEPTH,
};

struct mirror_highlight {
    uint64_t hash;
    float color[3];
};

struct mirror_config {
    enum mirror_mode mode;
    bool log;
    size_t highlight_count;
    struct mirror_highlight *highlights;
};

/* Reads configuration from the environment. Never fails: invalid values fall
 * back to defaults with a diagnostic on stderr. */
void mirror_config_load(struct mirror_config *cfg);

void mirror_config_free(struct mirror_config *cfg);

/* Returns the highlight entry for the given shader hash, or NULL. */
const struct mirror_highlight *mirror_config_find_highlight(const struct mirror_config *cfg,
                                                            uint64_t hash);

/* Parsing helpers, exposed for unit testing. */
bool mirror_config_parse_mode(const char *text, enum mirror_mode *mode);
bool mirror_config_parse_color(const char *text, size_t length, float color[3]);
bool mirror_config_parse_highlights(const char *text, struct mirror_config *cfg);

#endif
