#ifndef MIRROR_VKUTIL_H
#define MIRROR_VKUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/*
 * Minimal offscreen-rendering harness shared by the integration tests and
 * the example application. Works on any Vulkan 1.1 implementation including
 * CPU drivers such as lavapipe, so everything runs headless in CI.
 */

enum vku_result {
    VKU_OK = 0,
    VKU_NO_DEVICE, /* no usable Vulkan implementation: tests should skip */
    VKU_ERROR,
};

struct vku_ctx {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint64_t validation_errors;
};

/* One rectangle drawn with the given fragment shader at a fixed depth.
 * Geometry comes from a fullscreen triangle clipped by viewport/scissor;
 * `depth` is applied through the viewport depth range. */
struct vku_draw {
    const uint32_t *frag_spv;
    size_t frag_spv_size; /* in bytes */
    int32_t x, y;
    uint32_t width, height;
    float depth;
};

bool vku_layer_available(const char *layer_name);

enum vku_result vku_ctx_init(struct vku_ctx *ctx, bool enable_mirror_layer, bool enable_validation);
void vku_ctx_destroy(struct vku_ctx *ctx);

/* Renders the draw list into an RGBA8 image and reads it back into
 * `out_rgba` (width * height * 4 bytes, row-major). */
bool vku_render(struct vku_ctx *ctx, uint32_t width, uint32_t height, const struct vku_draw *draws,
                size_t draw_count, uint8_t *out_rgba);

#endif
