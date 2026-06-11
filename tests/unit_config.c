#define _DEFAULT_SOURCE /* setenv, unsetenv */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/hash.h"

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static int float_near(float a, float b) {
    float diff = a - b;
    return (diff < 0 ? -diff : diff) < 1e-6f;
}

static void test_parse_mode(void) {
    enum mirror_mode mode;
    CHECK(mirror_config_parse_mode("off", &mode) && mode == MIRROR_MODE_OFF);
    CHECK(mirror_config_parse_mode("0", &mode) && mode == MIRROR_MODE_OFF);
    CHECK(mirror_config_parse_mode("none", &mode) && mode == MIRROR_MODE_OFF);
    CHECK(mirror_config_parse_mode("depth", &mode) && mode == MIRROR_MODE_DEPTH);
    CHECK(mirror_config_parse_mode("on", &mode) && mode == MIRROR_MODE_DEPTH);
    CHECK(mirror_config_parse_mode("1", &mode) && mode == MIRROR_MODE_DEPTH);
    CHECK(!mirror_config_parse_mode("banana", &mode));
    CHECK(!mirror_config_parse_mode("", &mode));
}

static void test_parse_color(void) {
    float color[3];
    CHECK(mirror_config_parse_color("ff0000", 6, color) && float_near(color[0], 1.0f) &&
          float_near(color[1], 0.0f) && float_near(color[2], 0.0f));
    CHECK(mirror_config_parse_color("00FF00", 6, color) && float_near(color[1], 1.0f));
    CHECK(mirror_config_parse_color("000080", 6, color) && float_near(color[2], 128.0f / 255.0f));
    CHECK(!mirror_config_parse_color("ff000", 5, color));
    CHECK(!mirror_config_parse_color("gg0000", 6, color));
    CHECK(!mirror_config_parse_color("ff00001", 7, color));
}

static void test_parse_highlights(void) {
    struct mirror_config cfg = {0};

    CHECK(mirror_config_parse_highlights("deadbeefcafef00d=00ff00", &cfg));
    CHECK(cfg.highlight_count == 1);
    CHECK(cfg.highlights[0].hash == 0xdeadbeefcafef00dull);
    CHECK(float_near(cfg.highlights[0].color[1], 1.0f));

    CHECK(mirror_config_parse_highlights("1=ff0000,2=00ff00,3=0000ff", &cfg));
    CHECK(cfg.highlight_count == 3);
    CHECK(cfg.highlights[2].hash == 3);
    CHECK(mirror_config_find_highlight(&cfg, 2) != NULL);
    CHECK(mirror_config_find_highlight(&cfg, 4) == NULL);

    /* Invalid input must not clobber existing highlights. */
    CHECK(!mirror_config_parse_highlights("not-a-highlight", &cfg));
    CHECK(!mirror_config_parse_highlights("toolonghash00000000=ff0000", &cfg));
    CHECK(!mirror_config_parse_highlights("12=zzz", &cfg));
    CHECK(!mirror_config_parse_highlights("=ff0000", &cfg));
    CHECK(!mirror_config_parse_highlights("12=ff0000,", &cfg));
    CHECK(cfg.highlight_count == 3);

    mirror_config_free(&cfg);
}

static void test_load_from_env(void) {
    struct mirror_config cfg;

    unsetenv("MIRROR_MODE");
    unsetenv("MIRROR_HIGHLIGHT");
    unsetenv("MIRROR_LOG");
    mirror_config_load(&cfg);
    CHECK(cfg.mode == MIRROR_MODE_DEPTH);
    CHECK(!cfg.log);
    CHECK(cfg.highlight_count == 0);
    mirror_config_free(&cfg);

    setenv("MIRROR_MODE", "off", 1);
    setenv("MIRROR_LOG", "1", 1);
    setenv("MIRROR_HIGHLIGHT", "abc=123456", 1);
    mirror_config_load(&cfg);
    CHECK(cfg.mode == MIRROR_MODE_OFF);
    CHECK(cfg.log);
    CHECK(cfg.highlight_count == 1 && cfg.highlights[0].hash == 0xabc);
    mirror_config_free(&cfg);

    /* Garbage values fall back to defaults instead of failing. */
    setenv("MIRROR_MODE", "garbage", 1);
    setenv("MIRROR_LOG", "0", 1);
    setenv("MIRROR_HIGHLIGHT", "garbage", 1);
    mirror_config_load(&cfg);
    CHECK(cfg.mode == MIRROR_MODE_DEPTH);
    CHECK(!cfg.log);
    CHECK(cfg.highlight_count == 0);
    mirror_config_free(&cfg);

    unsetenv("MIRROR_MODE");
    unsetenv("MIRROR_HIGHLIGHT");
    unsetenv("MIRROR_LOG");
}

static void test_hash(void) {
    /* Known FNV-1a vectors. */
    CHECK(mirror_fnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(mirror_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(mirror_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
    CHECK(mirror_fnv1a64("foobaz", 6) != mirror_fnv1a64("foobar", 6));
}

int main(void) {
    test_parse_mode();
    test_parse_color();
    test_parse_highlights();
    test_load_from_env();
    test_hash();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("unit_config: all checks passed\n");
    return 0;
}
