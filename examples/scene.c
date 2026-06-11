/*
 * Renders a small game-like scene three times and writes PNG screenshots:
 *
 *   scene_normal.png    - without the mirror layer (original materials)
 *   scene_depth.png     - with the layer in depth mode
 *   scene_highlight.png - depth mode with the "player" material highlighted
 *                         in green and the "chest" material in yellow
 *
 * Usage: scene <output-directory>
 * Requires the layer manifest to be discoverable, e.g.:
 *   VK_ADD_LAYER_PATH=build/dev/layer.d ./build/dev/examples/scene docs/screenshots
 */

#define _DEFAULT_SOURCE /* setenv, unsetenv */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hash.h"
#include "../tests/vkutil.h"
#include "png.h"

#include "chest_frag_spv.h"
#include "floor_frag_spv.h"
#include "player_frag_spv.h"
#include "sky_frag_spv.h"
#include "wall_frag_spv.h"

enum { WIDTH = 512, HEIGHT = 512 };

#define DRAW(shader, dx, dy, dw, dh, dz)                                                           \
    {                                                                                              \
        .frag_spv = shader, .frag_spv_size = sizeof shader, .x = dx, .y = dy, .width = dw,         \
        .height = dh, .depth = dz                                                                  \
    }

/* A crude corridor: walls receding into the distance, two "players" and a
 * "chest" placed at different depths. */
static const struct vku_draw scene_draws[] = {
    DRAW(sky_frag_spv, 0, 0, 512, 512, 0.98f),      DRAW(floor_frag_spv, 0, 320, 512, 192, 0.90f),
    DRAW(wall_frag_spv, 0, 64, 96, 320, 0.85f),     DRAW(wall_frag_spv, 416, 64, 96, 320, 0.85f),
    DRAW(wall_frag_spv, 96, 112, 64, 240, 0.70f),   DRAW(wall_frag_spv, 352, 112, 64, 240, 0.70f),
    DRAW(chest_frag_spv, 296, 296, 72, 56, 0.55f),  DRAW(player_frag_spv, 200, 200, 48, 168, 0.45f),
    DRAW(player_frag_spv, 96, 240, 64, 200, 0.30f),
};

static int render_to_png(bool enable_layer, const char *path) {
    struct vku_ctx ctx;
    enum vku_result result = vku_ctx_init(&ctx, enable_layer, false);
    if (result == VKU_NO_DEVICE) {
        fprintf(stderr, "no Vulkan device available\n");
        return 77;
    }
    if (result != VKU_OK) {
        fprintf(stderr, "context creation failed; is VK_ADD_LAYER_PATH set?\n");
        return 1;
    }

    int status = 1;
    uint8_t *image = malloc((size_t)WIDTH * HEIGHT * 4);
    if (image &&
        vku_render(&ctx, WIDTH, HEIGHT, scene_draws, sizeof scene_draws / sizeof scene_draws[0],
                   image) &&
        png_write_rgba(path, WIDTH, HEIGHT, image)) {
        printf("wrote %s\n", path);
        status = 0;
    } else {
        fprintf(stderr, "failed to render %s\n", path);
    }
    free(image);
    vku_ctx_destroy(&ctx);
    return status;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    const char *out_dir = argv[1];
    char path[1024];
    int status;

    unsetenv("MIRROR_MODE");
    unsetenv("MIRROR_HIGHLIGHT");

    snprintf(path, sizeof path, "%s/scene_normal.png", out_dir);
    if ((status = render_to_png(false, path)) != 0)
        return status;

    setenv("MIRROR_MODE", "depth", 1);
    snprintf(path, sizeof path, "%s/scene_depth.png", out_dir);
    if ((status = render_to_png(true, path)) != 0)
        return status;

    char highlights[160];
    snprintf(highlights, sizeof highlights, "%016" PRIx64 "=00ff00,%016" PRIx64 "=ffcc00",
             mirror_fnv1a64(player_frag_spv, sizeof player_frag_spv),
             mirror_fnv1a64(chest_frag_spv, sizeof chest_frag_spv));
    setenv("MIRROR_HIGHLIGHT", highlights, 1);
    printf("MIRROR_HIGHLIGHT=%s\n", highlights);
    snprintf(path, sizeof path, "%s/scene_highlight.png", out_dir);
    return render_to_png(true, path);
}
