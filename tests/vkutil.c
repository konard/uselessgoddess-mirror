#include "vkutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fullscreen_vert_spv.h"

#define MIRROR_LAYER_NAME     "VK_LAYER_MIRROR_mirror"
#define VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"

bool vku_layer_available(const char *layer_name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkLayerProperties *properties = calloc(count, sizeof *properties);
    if (!properties)
        return false;
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&count, properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++)
            if (strcmp(properties[i].layerName, layer_name) == 0)
                found = true;
    free(properties);
    return found;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) {
    (void)types;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        uint64_t *errors = user_data;
        (*errors)++;
        fprintf(stderr, "[validation] %s\n", callback_data->pMessage);
    }
    return VK_FALSE;
}

enum vku_result vku_ctx_init(struct vku_ctx *ctx, bool enable_mirror_layer,
                             bool enable_validation) {
    *ctx = (struct vku_ctx){0};

    const char *layers[2];
    uint32_t layer_count = 0;
    if (enable_mirror_layer)
        layers[layer_count++] = MIRROR_LAYER_NAME;
    if (enable_validation)
        layers[layer_count++] = VALIDATION_LAYER_NAME;

    const char *extensions[1];
    uint32_t extension_count = 0;
    if (enable_validation)
        extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mirror-vkutil",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };

    VkResult result = vkCreateInstance(&instance_info, NULL, &ctx->instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
        return VKU_NO_DEVICE;
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return VKU_ERROR;
    }

    if (enable_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                ctx->instance, "vkCreateDebugUtilsMessengerEXT");
        const VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData = &ctx->validation_errors,
        };
        if (!create_messenger ||
            create_messenger(ctx->instance, &messenger_info, NULL, &ctx->messenger) != VK_SUCCESS) {
            fprintf(stderr, "failed to create debug messenger\n");
            vku_ctx_destroy(ctx);
            return VKU_ERROR;
        }
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, NULL);
    if (device_count == 0) {
        vku_ctx_destroy(ctx);
        return VKU_NO_DEVICE;
    }
    VkPhysicalDevice *devices = calloc(device_count, sizeof *devices);
    if (!devices) {
        vku_ctx_destroy(ctx);
        return VKU_ERROR;
    }
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);

    for (uint32_t i = 0; i < device_count && !ctx->physical_device; i++) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, NULL);
        VkQueueFamilyProperties *families = calloc(family_count, sizeof *families);
        if (!families)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (uint32_t f = 0; f < family_count; f++) {
            if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                ctx->physical_device = devices[i];
                ctx->queue_family = f;
                break;
            }
        }
        free(families);
    }
    free(devices);
    if (!ctx->physical_device) {
        vku_ctx_destroy(ctx);
        return VKU_NO_DEVICE;
    }

    const float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    if (vkCreateDevice(ctx->physical_device, &device_info, NULL, &ctx->device) != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed\n");
        vku_ctx_destroy(ctx);
        return VKU_ERROR;
    }
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
    return VKU_OK;
}

void vku_ctx_destroy(struct vku_ctx *ctx) {
    if (ctx->device)
        vkDestroyDevice(ctx->device, NULL);
    if (ctx->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                ctx->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger)
            destroy_messenger(ctx->instance, ctx->messenger, NULL);
    }
    if (ctx->instance)
        vkDestroyInstance(ctx->instance, NULL);
    *ctx = (struct vku_ctx){0};
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                                 VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return UINT32_MAX;
}

struct render_objects {
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    VkShaderModule vertex_module;
    VkShaderModule *fragment_modules;
    VkPipelineLayout layout;
    VkPipeline *pipelines;
    VkCommandPool pool;
    size_t draw_count;
};

static void render_objects_destroy(struct vku_ctx *ctx, struct render_objects *objects) {
    VkDevice device = ctx->device;
    if (objects->pool)
        vkDestroyCommandPool(device, objects->pool, NULL);
    if (objects->pipelines) {
        for (size_t i = 0; i < objects->draw_count; i++)
            if (objects->pipelines[i])
                vkDestroyPipeline(device, objects->pipelines[i], NULL);
        free(objects->pipelines);
    }
    if (objects->layout)
        vkDestroyPipelineLayout(device, objects->layout, NULL);
    if (objects->fragment_modules) {
        for (size_t i = 0; i < objects->draw_count; i++)
            if (objects->fragment_modules[i])
                vkDestroyShaderModule(device, objects->fragment_modules[i], NULL);
        free(objects->fragment_modules);
    }
    if (objects->vertex_module)
        vkDestroyShaderModule(device, objects->vertex_module, NULL);
    if (objects->readback)
        vkDestroyBuffer(device, objects->readback, NULL);
    if (objects->readback_memory)
        vkFreeMemory(device, objects->readback_memory, NULL);
    if (objects->framebuffer)
        vkDestroyFramebuffer(device, objects->framebuffer, NULL);
    if (objects->render_pass)
        vkDestroyRenderPass(device, objects->render_pass, NULL);
    if (objects->view)
        vkDestroyImageView(device, objects->view, NULL);
    if (objects->image)
        vkDestroyImage(device, objects->image, NULL);
    if (objects->image_memory)
        vkFreeMemory(device, objects->image_memory, NULL);
    *objects = (struct render_objects){0};
}

#define VKU_CHECK(call)                                                                            \
    do {                                                                                           \
        VkResult vku_check_result = (call);                                                        \
        if (vku_check_result != VK_SUCCESS) {                                                      \
            fprintf(stderr, "%s failed: %d (%s:%d)\n", #call, vku_check_result, __FILE__,          \
                    __LINE__);                                                                     \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

bool vku_render(struct vku_ctx *ctx, uint32_t width, uint32_t height, const struct vku_draw *draws,
                size_t draw_count, uint8_t *out_rgba) {
    struct render_objects objects = {.draw_count = draw_count};
    VkDevice device = ctx->device;
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize byte_size = (VkDeviceSize)width * height * 4;

    /* Color target. */
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VKU_CHECK(vkCreateImage(device, &image_info, NULL, &objects.image));

    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, objects.image, &image_requirements);
    uint32_t image_memory_type =
        find_memory_type(ctx->physical_device, image_requirements.memoryTypeBits, 0);
    if (image_memory_type == UINT32_MAX)
        goto fail;
    const VkMemoryAllocateInfo image_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_requirements.size,
        .memoryTypeIndex = image_memory_type,
    };
    VKU_CHECK(vkAllocateMemory(device, &image_alloc, NULL, &objects.image_memory));
    VKU_CHECK(vkBindImageMemory(device, objects.image, objects.image_memory, 0));

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = objects.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VKU_CHECK(vkCreateImageView(device, &view_info, NULL, &objects.view));

    /* Render pass: clear to black, end in TRANSFER_SRC for the readback. */
    const VkAttachmentDescription attachment = {
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference color_reference = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };
    const VkSubpassDependency dependency = {
        .srcSubpass = 0,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    const VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VKU_CHECK(vkCreateRenderPass(device, &render_pass_info, NULL, &objects.render_pass));

    const VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = objects.render_pass,
        .attachmentCount = 1,
        .pAttachments = &objects.view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    VKU_CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &objects.framebuffer));

    /* Host-visible readback buffer. */
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = byte_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VKU_CHECK(vkCreateBuffer(device, &buffer_info, NULL, &objects.readback));

    VkMemoryRequirements buffer_requirements;
    vkGetBufferMemoryRequirements(device, objects.readback, &buffer_requirements);
    uint32_t buffer_memory_type = find_memory_type(
        ctx->physical_device, buffer_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buffer_memory_type == UINT32_MAX)
        goto fail;
    const VkMemoryAllocateInfo buffer_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_requirements.size,
        .memoryTypeIndex = buffer_memory_type,
    };
    VKU_CHECK(vkAllocateMemory(device, &buffer_alloc, NULL, &objects.readback_memory));
    VKU_CHECK(vkBindBufferMemory(device, objects.readback, objects.readback_memory, 0));

    /* Shader modules and pipelines: one pipeline per draw, created in a
     * single vkCreateGraphicsPipelines call to exercise batch patching. */
    const VkShaderModuleCreateInfo vertex_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof fullscreen_vert_spv,
        .pCode = fullscreen_vert_spv,
    };
    VKU_CHECK(vkCreateShaderModule(device, &vertex_info, NULL, &objects.vertex_module));

    objects.fragment_modules = calloc(draw_count, sizeof *objects.fragment_modules);
    objects.pipelines = calloc(draw_count, sizeof *objects.pipelines);
    if (!objects.fragment_modules || !objects.pipelines)
        goto fail;

    for (size_t i = 0; i < draw_count; i++) {
        const VkShaderModuleCreateInfo fragment_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = draws[i].frag_spv_size,
            .pCode = draws[i].frag_spv,
        };
        VKU_CHECK(vkCreateShaderModule(device, &fragment_info, NULL, &objects.fragment_modules[i]));
    }

    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VKU_CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &objects.layout));

    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkGraphicsPipelineCreateInfo *pipeline_infos = calloc(draw_count, sizeof *pipeline_infos);
    VkPipelineShaderStageCreateInfo(*stages)[2] = calloc(draw_count, sizeof *stages);
    if (!pipeline_infos || !stages) {
        free(pipeline_infos);
        free(stages);
        goto fail;
    }
    for (size_t i = 0; i < draw_count; i++) {
        stages[i][0] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = objects.vertex_module,
            .pName = "main",
        };
        stages[i][1] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = objects.fragment_modules[i],
            .pName = "main",
        };
        pipeline_infos[i] = (VkGraphicsPipelineCreateInfo){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = stages[i],
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic_state,
            .layout = objects.layout,
            .renderPass = objects.render_pass,
            .subpass = 0,
        };
    }
    VkResult pipelines_result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, (uint32_t)draw_count, pipeline_infos, NULL, objects.pipelines);
    free(pipeline_infos);
    free(stages);
    if (pipelines_result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed: %d\n", pipelines_result);
        goto fail;
    }

    /* Record and submit. */
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = ctx->queue_family,
    };
    VKU_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &objects.pool));

    VkCommandBuffer command_buffer;
    const VkCommandBufferAllocateInfo command_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = objects.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VKU_CHECK(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer));

    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VKU_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    const VkClearValue clear_value = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    const VkRenderPassBeginInfo render_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = objects.render_pass,
        .framebuffer = objects.framebuffer,
        .renderArea = {{0, 0}, {width, height}},
        .clearValueCount = 1,
        .pClearValues = &clear_value,
    };
    vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    for (size_t i = 0; i < draw_count; i++) {
        const VkViewport viewport = {
            .x = (float)draws[i].x,
            .y = (float)draws[i].y,
            .width = (float)draws[i].width,
            .height = (float)draws[i].height,
            .minDepth = draws[i].depth,
            .maxDepth = draws[i].depth,
        };
        const VkRect2D scissor = {
            .offset = {draws[i].x, draws[i].y},
            .extent = {draws[i].width, draws[i].height},
        };
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, objects.pipelines[i]);
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command_buffer);

    const VkBufferImageCopy copy_region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyImageToBuffer(command_buffer, objects.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           objects.readback, 1, &copy_region);
    VKU_CHECK(vkEndCommandBuffer(command_buffer));

    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };
    VKU_CHECK(vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE));
    VKU_CHECK(vkQueueWaitIdle(ctx->queue));

    void *mapped = NULL;
    VKU_CHECK(vkMapMemory(device, objects.readback_memory, 0, byte_size, 0, &mapped));
    memcpy(out_rgba, mapped, byte_size);
    vkUnmapMemory(device, objects.readback_memory);

    render_objects_destroy(ctx, &objects);
    return true;

fail:
    render_objects_destroy(ctx, &objects);
    return false;
}
