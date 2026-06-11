/*
 * End-to-end test of the mirror layer on a real Vulkan implementation
 * (a CPU driver such as lavapipe is enough, so this runs headless in CI).
 *
 * The scene is two half-screen rectangles:
 *   left  - "red material"  at depth 0.25
 *   right - "blue material" at depth 0.75
 *
 * Scenarios (selected by argv[1]):
 *   enumerate  - the layer is visible to vkEnumerateInstanceLayerProperties
 *   nolayer    - baseline without the layer: original material colors
 *   off        - layer enabled but MIRROR_MODE=off: passthrough
 *   depth      - depth visualization: grayscale equal to each draw's depth
 *   highlight  - the blue material is highlighted in green via its hash
 *   validation - depth mode under VK_LAYER_KHRONOS_validation: zero errors
 *
 * Exit codes: 0 = pass, 1 = fail, 77 = skip (no Vulkan device available).
 */

#define _DEFAULT_SOURCE /* setenv, unsetenv */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hash.h"
#include "vkutil.h"

#include "blue_frag_spv.h"
#include "red_frag_spv.h"

#define MIRROR_LAYER_NAME "VK_LAYER_MIRROR_mirror"

enum { WIDTH = 256, HEIGHT = 256 };

struct pixel {
    uint8_t r, g, b, a;
};

static struct pixel pixel_at(const uint8_t *image, uint32_t x, uint32_t y) {
    const uint8_t *p = image + (y * WIDTH + x) * 4;
    return (struct pixel){p[0], p[1], p[2], p[3]};
}

static int channel_near(uint8_t actual, int expected, int tolerance) {
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

static int expect_pixel(const char *what, struct pixel p, int r, int g, int b) {
    const int tolerance = 3;
    if (channel_near(p.r, r, tolerance) && channel_near(p.g, g, tolerance) &&
        channel_near(p.b, b, tolerance) && p.a == 255)
        return 1;
    fprintf(stderr, "%s: expected ~(%d, %d, %d, 255), got (%d, %d, %d, %d)\n", what, r, g, b, p.r,
            p.g, p.b, p.a);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s enumerate|nolayer|off|depth|highlight|validation\n", argv[0]);
        return 1;
    }
    const char *scenario = argv[1];

    /* Each scenario fully controls the layer configuration. */
    unsetenv("MIRROR_MODE");
    unsetenv("MIRROR_HIGHLIGHT");
    unsetenv("MIRROR_LOG");

    if (strcmp(scenario, "enumerate") == 0) {
        if (!vku_layer_available(MIRROR_LAYER_NAME)) {
            fprintf(stderr, "layer %s not found; is VK_ADD_LAYER_PATH set?\n", MIRROR_LAYER_NAME);
            return 1;
        }
        printf("enumerate: layer is visible to the loader\n");
        return 0;
    }

    bool enable_mirror = strcmp(scenario, "nolayer") != 0;
    bool enable_validation = strcmp(scenario, "validation") == 0;

    if (strcmp(scenario, "off") == 0)
        setenv("MIRROR_MODE", "off", 1);
    else
        setenv("MIRROR_MODE", "depth", 1);
    if (strcmp(scenario, "highlight") == 0) {
        char highlight[64];
        snprintf(highlight, sizeof highlight, "%016" PRIx64 "=00ff00",
                 mirror_fnv1a64(blue_frag_spv, sizeof blue_frag_spv));
        setenv("MIRROR_HIGHLIGHT", highlight, 1);
    }

    struct vku_ctx ctx;
    enum vku_result init_result = vku_ctx_init(&ctx, enable_mirror, enable_validation);
    if (init_result == VKU_NO_DEVICE) {
        fprintf(stderr, "no Vulkan device available, skipping\n");
        return 77;
    }
    if (init_result != VKU_OK)
        return 1;

    const struct vku_draw draws[] = {
        {
            .frag_spv = red_frag_spv,
            .frag_spv_size = sizeof red_frag_spv,
            .x = 0,
            .y = 0,
            .width = WIDTH / 2,
            .height = HEIGHT,
            .depth = 0.25f,
        },
        {
            .frag_spv = blue_frag_spv,
            .frag_spv_size = sizeof blue_frag_spv,
            .x = WIDTH / 2,
            .y = 0,
            .width = WIDTH / 2,
            .height = HEIGHT,
            .depth = 0.75f,
        },
    };

    uint8_t *image = malloc((size_t)WIDTH * HEIGHT * 4);
    if (!image)
        return 1;
    if (!vku_render(&ctx, WIDTH, HEIGHT, draws, 2, image)) {
        free(image);
        vku_ctx_destroy(&ctx);
        return 1;
    }

    struct pixel left = pixel_at(image, WIDTH / 4, HEIGHT / 2);
    struct pixel right = pixel_at(image, 3 * WIDTH / 4, HEIGHT / 2);
    free(image);

    int ok = 1;
    if (strcmp(scenario, "nolayer") == 0 || strcmp(scenario, "off") == 0) {
        ok &= expect_pixel("left (red material)", left, 255, 0, 0);
        ok &= expect_pixel("right (blue material)", right, 0, 0, 255);
    } else if (strcmp(scenario, "depth") == 0 || strcmp(scenario, "validation") == 0) {
        /* Depth 0.25 -> 64/255, depth 0.75 -> 191/255. */
        ok &= expect_pixel("left (depth 0.25)", left, 64, 64, 64);
        ok &= expect_pixel("right (depth 0.75)", right, 191, 191, 191);
    } else if (strcmp(scenario, "highlight") == 0) {
        /* Highlighted color is scaled by 1 - 0.5 * depth = 0.625 -> 159/255. */
        ok &= expect_pixel("left (depth 0.25)", left, 64, 64, 64);
        ok &= expect_pixel("right (green highlight)", right, 0, 159, 0);
    } else {
        fprintf(stderr, "unknown scenario '%s'\n", scenario);
        ok = 0;
    }

    if (enable_validation && ctx.validation_errors != 0) {
        fprintf(stderr, "validation reported %" PRIu64 " error(s)\n", ctx.validation_errors);
        ok = 0;
    }

    vku_ctx_destroy(&ctx);
    if (ok)
        printf("%s: passed\n", scenario);
    return ok ? 0 : 1;
}
