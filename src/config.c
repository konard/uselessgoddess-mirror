#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mirror_config_parse_mode(const char *text, enum mirror_mode *mode) {
    if (strcmp(text, "off") == 0 || strcmp(text, "0") == 0 || strcmp(text, "none") == 0) {
        *mode = MIRROR_MODE_OFF;
        return true;
    }
    if (strcmp(text, "depth") == 0 || strcmp(text, "on") == 0 || strcmp(text, "1") == 0) {
        *mode = MIRROR_MODE_DEPTH;
        return true;
    }
    return false;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool mirror_config_parse_color(const char *text, size_t length, float color[3]) {
    if (length != 6)
        return false;
    for (size_t channel = 0; channel < 3; channel++) {
        int high = hex_digit(text[channel * 2]);
        int low = hex_digit(text[channel * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        color[channel] = (float)(high * 16 + low) / 255.0f;
    }
    return true;
}

static bool parse_highlight_entry(const char *entry, size_t length, struct mirror_highlight *out) {
    const char *eq = memchr(entry, '=', length);
    if (!eq)
        return false;
    size_t hash_len = (size_t)(eq - entry);
    if (hash_len == 0 || hash_len > 16)
        return false;
    uint64_t hash = 0;
    for (size_t i = 0; i < hash_len; i++) {
        int digit = hex_digit(entry[i]);
        if (digit < 0)
            return false;
        hash = hash << 4 | (uint64_t)digit;
    }
    float color[3];
    if (!mirror_config_parse_color(eq + 1, length - hash_len - 1, color))
        return false;
    out->hash = hash;
    memcpy(out->color, color, sizeof color);
    return true;
}

bool mirror_config_parse_highlights(const char *text, struct mirror_config *cfg) {
    size_t count = 1;
    for (const char *c = text; *c; c++)
        if (*c == ',')
            count++;

    struct mirror_highlight *highlights = calloc(count, sizeof *highlights);
    if (!highlights)
        return false;

    size_t parsed = 0;
    const char *cursor = text;
    while (true) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (!parse_highlight_entry(cursor, length, &highlights[parsed])) {
            free(highlights);
            return false;
        }
        parsed++;
        if (!comma)
            break;
        cursor = comma + 1;
    }

    free(cfg->highlights);
    cfg->highlights = highlights;
    cfg->highlight_count = parsed;
    return true;
}

void mirror_config_load(struct mirror_config *cfg) {
    *cfg = (struct mirror_config){.mode = MIRROR_MODE_DEPTH};

    const char *mode = getenv("MIRROR_MODE");
    if (mode && !mirror_config_parse_mode(mode, &cfg->mode))
        fprintf(stderr, "[mirror] unknown MIRROR_MODE '%s', defaulting to 'depth'\n", mode);

    const char *log = getenv("MIRROR_LOG");
    cfg->log = log && *log && strcmp(log, "0") != 0;

    const char *highlight = getenv("MIRROR_HIGHLIGHT");
    if (highlight && *highlight && !mirror_config_parse_highlights(highlight, cfg))
        fprintf(stderr,
                "[mirror] invalid MIRROR_HIGHLIGHT '%s', expected "
                "<hash>=<RRGGBB>[,<hash>=<RRGGBB>...]\n",
                highlight);
}

void mirror_config_free(struct mirror_config *cfg) {
    free(cfg->highlights);
    *cfg = (struct mirror_config){0};
}

const struct mirror_highlight *mirror_config_find_highlight(const struct mirror_config *cfg,
                                                            uint64_t hash) {
    for (size_t i = 0; i < cfg->highlight_count; i++)
        if (cfg->highlights[i].hash == hash)
            return &cfg->highlights[i];
    return NULL;
}
