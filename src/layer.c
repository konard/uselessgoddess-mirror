/*
 * mirror - a Vulkan layer that replaces every material with a depth
 * visualization, optionally highlighting selected materials in a
 * configurable color.
 *
 * How it works:
 *  - vkCreateShaderModule is intercepted to remember the FNV-1a hash of each
 *    module's SPIR-V, so fragment shaders ("materials") can be identified.
 *  - vkCreateGraphicsPipelines is intercepted to swap the application's
 *    fragment shader for a tiny replacement shader that outputs the
 *    window-space depth as grayscale, or a highlight color selected through
 *    specialization constants.
 *
 * The layer never wraps Vulkan handles; it only keeps side tables keyed by
 * the loader's dispatch keys, which keeps it transparent to applications
 * and other layers.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "config.h"
#include "hash.h"
#include "umap.h"

#include "mirror_frag_spv.h"

#define MIRROR_LAYER_NAME        "VK_LAYER_MIRROR_mirror"
#define MIRROR_LAYER_DESCRIPTION "Renders depth instead of materials, with configurable highlights"
#define MIRROR_SPEC_VERSION      VK_MAKE_API_VERSION(0, 1, 3, 0)
#define MIRROR_IMPL_VERSION      1

#define MIRROR_EXPORT __attribute__((visibility("default")))

struct instance_data {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    struct instance_data *next;
};

struct device_dispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
};

struct device_data {
    void *key;
    VkDevice device;
    struct device_dispatch vt;
    struct mirror_config cfg;
    mtx_t lock;                       /* guards module_hashes and mirror_module */
    struct mirror_umap module_hashes; /* VkShaderModule handle -> SPIR-V hash */
    VkShaderModule mirror_module;     /* lazily created replacement shader */
    struct device_data *next;
};

static mtx_t g_lock;
static once_flag g_once = ONCE_FLAG_INIT;
static struct instance_data *g_instances;
static struct device_data *g_devices;

static void global_init(void) {
    mtx_init(&g_lock, mtx_plain);
}

/* The loader writes a dispatch table pointer into the first field of every
 * dispatchable handle; it uniquely identifies the instance/device chain. */
static inline void *dispatch_key(const void *dispatchable_handle) {
    return *(void *const *)dispatchable_handle;
}

static struct instance_data *instance_find(void *key) {
    mtx_lock(&g_lock);
    struct instance_data *inst = g_instances;
    while (inst && inst->key != key)
        inst = inst->next;
    mtx_unlock(&g_lock);
    return inst;
}

static struct device_data *device_find(void *key) {
    mtx_lock(&g_lock);
    struct device_data *dev = g_devices;
    while (dev && dev->key != key)
        dev = dev->next;
    mtx_unlock(&g_lock);
    return dev;
}

static void mirror_log(const struct mirror_config *cfg, const char *format, ...) {
    if (!cfg->log)
        return;
    va_list args;
    va_start(args, format);
    fputs("[mirror] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

/* ---------------------------------------------------------------- instance */

static VkLayerInstanceCreateInfo *find_instance_link_info(const VkInstanceCreateInfo *create_info) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)create_info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    return chain;
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_CreateInstance(const VkInstanceCreateInfo *create_info,
                                                            const VkAllocationCallbacks *allocator,
                                                            VkInstance *instance) {
    call_once(&g_once, global_init);

    VkLayerInstanceCreateInfo *chain = find_instance_link_info(create_info);
    if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Advance the chain for the layers below us. */
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    VkResult result = next_create(create_info, allocator, instance);
    if (result != VK_SUCCESS)
        return result;

    struct instance_data *inst = calloc(1, sizeof *inst);
    if (!inst) {
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)next_gipa(*instance, "vkDestroyInstance");
        if (destroy)
            destroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    inst->key = dispatch_key(*instance);
    inst->instance = *instance;
    inst->GetInstanceProcAddr = next_gipa;
    inst->DestroyInstance = (PFN_vkDestroyInstance)next_gipa(*instance, "vkDestroyInstance");
    inst->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(
        *instance, "vkEnumerateDeviceExtensionProperties");

    mtx_lock(&g_lock);
    inst->next = g_instances;
    g_instances = inst;
    mtx_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mirror_DestroyInstance(VkInstance instance,
                                                         const VkAllocationCallbacks *allocator) {
    if (!instance)
        return;
    void *key = dispatch_key(instance);

    mtx_lock(&g_lock);
    struct instance_data **link = &g_instances;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    struct instance_data *inst = *link;
    if (inst)
        *link = inst->next;
    mtx_unlock(&g_lock);

    if (!inst)
        return;
    inst->DestroyInstance(instance, allocator);
    free(inst);
}

/* ------------------------------------------------------------------ device */

static VkLayerDeviceCreateInfo *find_device_link_info(const VkDeviceCreateInfo *create_info) {
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)create_info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    return chain;
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_CreateDevice(VkPhysicalDevice physical_device,
                                                          const VkDeviceCreateInfo *create_info,
                                                          const VkAllocationCallbacks *allocator,
                                                          VkDevice *device) {
    call_once(&g_once, global_init);

    VkLayerDeviceCreateInfo *chain = find_device_link_info(create_info);
    if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    struct instance_data *inst = instance_find(dispatch_key(physical_device));
    VkInstance instance_handle = inst ? inst->instance : NULL;
    PFN_vkCreateDevice next_create =
        (PFN_vkCreateDevice)next_gipa(instance_handle, "vkCreateDevice");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    VkResult result = next_create(physical_device, create_info, allocator, device);
    if (result != VK_SUCCESS)
        return result;

    struct device_data *dev = calloc(1, sizeof *dev);
    if (!dev) {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)next_gdpa(*device, "vkDestroyDevice");
        if (destroy)
            destroy(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    dev->key = dispatch_key(*device);
    dev->device = *device;
    dev->vt.GetDeviceProcAddr = next_gdpa;
    dev->vt.DestroyDevice = (PFN_vkDestroyDevice)next_gdpa(*device, "vkDestroyDevice");
    dev->vt.CreateShaderModule =
        (PFN_vkCreateShaderModule)next_gdpa(*device, "vkCreateShaderModule");
    dev->vt.DestroyShaderModule =
        (PFN_vkDestroyShaderModule)next_gdpa(*device, "vkDestroyShaderModule");
    dev->vt.CreateGraphicsPipelines =
        (PFN_vkCreateGraphicsPipelines)next_gdpa(*device, "vkCreateGraphicsPipelines");
    mtx_init(&dev->lock, mtx_plain);
    mirror_umap_init(&dev->module_hashes);
    mirror_config_load(&dev->cfg);

    mirror_log(&dev->cfg, "device created, mode=%s, %zu highlight(s)",
               dev->cfg.mode == MIRROR_MODE_DEPTH ? "depth" : "off", dev->cfg.highlight_count);

    mtx_lock(&g_lock);
    dev->next = g_devices;
    g_devices = dev;
    mtx_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mirror_DestroyDevice(VkDevice device,
                                                       const VkAllocationCallbacks *allocator) {
    if (!device)
        return;
    void *key = dispatch_key(device);

    mtx_lock(&g_lock);
    struct device_data **link = &g_devices;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    struct device_data *dev = *link;
    if (dev)
        *link = dev->next;
    mtx_unlock(&g_lock);

    if (!dev)
        return;
    if (dev->mirror_module != VK_NULL_HANDLE)
        dev->vt.DestroyShaderModule(device, dev->mirror_module, NULL);
    dev->vt.DestroyDevice(device, allocator);
    mirror_umap_free(&dev->module_hashes);
    mirror_config_free(&dev->cfg);
    mtx_destroy(&dev->lock);
    free(dev);
}

/* --------------------------------------------------------- shader tracking */

static VKAPI_ATTR VkResult VKAPI_CALL
mirror_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *create_info,
                          const VkAllocationCallbacks *allocator, VkShaderModule *shader_module) {
    struct device_data *dev = device_find(dispatch_key(device));
    VkResult result = dev->vt.CreateShaderModule(device, create_info, allocator, shader_module);
    if (result != VK_SUCCESS)
        return result;

    uint64_t hash = mirror_fnv1a64(create_info->pCode, create_info->codeSize);
    mtx_lock(&dev->lock);
    mirror_umap_put(&dev->module_hashes, (uint64_t)(uintptr_t)*shader_module, hash);
    mtx_unlock(&dev->lock);

    mirror_log(&dev->cfg, "shader module created, hash=%016" PRIx64 " (%zu bytes)", hash,
               create_info->codeSize);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mirror_DestroyShaderModule(
    VkDevice device, VkShaderModule shader_module, const VkAllocationCallbacks *allocator) {
    struct device_data *dev = device_find(dispatch_key(device));
    if (shader_module != VK_NULL_HANDLE) {
        mtx_lock(&dev->lock);
        mirror_umap_remove(&dev->module_hashes, (uint64_t)(uintptr_t)shader_module);
        mtx_unlock(&dev->lock);
    }
    dev->vt.DestroyShaderModule(device, shader_module, allocator);
}

/* ------------------------------------------------------ pipeline patching */

struct spec_payload {
    uint32_t mode; /* 0 = depth grayscale, 1 = highlight color */
    float color[3];
};

static const VkSpecializationMapEntry spec_entries[] = {
    {.constantID = 0, .offset = offsetof(struct spec_payload, mode), .size = sizeof(uint32_t)},
    {.constantID = 1, .offset = offsetof(struct spec_payload, color[0]), .size = sizeof(float)},
    {.constantID = 2, .offset = offsetof(struct spec_payload, color[1]), .size = sizeof(float)},
    {.constantID = 3, .offset = offsetof(struct spec_payload, color[2]), .size = sizeof(float)},
};

struct pipeline_patch {
    VkPipelineShaderStageCreateInfo *stages;
    VkSpecializationInfo spec_info;
    struct spec_payload payload;
};

static VkResult ensure_mirror_module(struct device_data *dev) {
    VkResult result = VK_SUCCESS;
    mtx_lock(&dev->lock);
    if (dev->mirror_module == VK_NULL_HANDLE) {
        const VkShaderModuleCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof mirror_frag_spv,
            .pCode = mirror_frag_spv,
        };
        result = dev->vt.CreateShaderModule(dev->device, &create_info, NULL, &dev->mirror_module);
    }
    mtx_unlock(&dev->lock);
    return result;
}

/* Determines the SPIR-V hash of a fragment stage, either through the module
 * registry or from an inline VkShaderModuleCreateInfo (VK_KHR_maintenance5 /
 * VK_EXT_graphics_pipeline_library style). */
static bool fragment_stage_hash(struct device_data *dev,
                                const VkPipelineShaderStageCreateInfo *stage, uint64_t *hash) {
    if (stage->module != VK_NULL_HANDLE) {
        mtx_lock(&dev->lock);
        bool found = mirror_umap_get(&dev->module_hashes, (uint64_t)(uintptr_t)stage->module, hash);
        mtx_unlock(&dev->lock);
        return found;
    }
    for (const VkBaseInStructure *s = stage->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *inline_module = (const VkShaderModuleCreateInfo *)s;
            *hash = mirror_fnv1a64(inline_module->pCode, inline_module->codeSize);
            return true;
        }
    }
    return false;
}

static void patch_pipeline(struct device_data *dev, VkGraphicsPipelineCreateInfo *info,
                           struct pipeline_patch *patch) {
    if (!info->pStages || info->stageCount == 0)
        return;

    uint32_t fragment_index = UINT32_MAX;
    for (uint32_t i = 0; i < info->stageCount; i++)
        if (info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
            fragment_index = i;
    if (fragment_index == UINT32_MAX)
        return; /* depth-only or compute-like pipeline: nothing to replace */

    VkPipelineShaderStageCreateInfo *stages = malloc(info->stageCount * sizeof *stages);
    if (!stages)
        return; /* out of memory: leave this pipeline untouched */
    memcpy(stages, info->pStages, info->stageCount * sizeof *stages);

    uint64_t hash = 0;
    bool hashed = fragment_stage_hash(dev, &stages[fragment_index], &hash);
    const struct mirror_highlight *highlight =
        hashed ? mirror_config_find_highlight(&dev->cfg, hash) : NULL;

    patch->payload = (struct spec_payload){.mode = highlight ? 1u : 0u};
    if (highlight)
        memcpy(patch->payload.color, highlight->color, sizeof patch->payload.color);
    patch->spec_info = (VkSpecializationInfo){
        .mapEntryCount = sizeof spec_entries / sizeof spec_entries[0],
        .pMapEntries = spec_entries,
        .dataSize = sizeof patch->payload,
        .pData = &patch->payload,
    };

    /* The original stage's pNext described the shader we are replacing, so it
     * is intentionally dropped along with the original specialization data. */
    stages[fragment_index] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = dev->mirror_module,
        .pName = "main",
        .pSpecializationInfo = &patch->spec_info,
    };

    patch->stages = stages;
    info->pStages = stages;

    if (hashed)
        mirror_log(&dev->cfg, "pipeline patched, fragment hash=%016" PRIx64 "%s", hash,
                   highlight ? " (highlighted)" : "");
    else
        mirror_log(&dev->cfg, "pipeline patched, fragment shader unidentified");
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache, uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos, const VkAllocationCallbacks *allocator,
    VkPipeline *pipelines) {
    struct device_data *dev = device_find(dispatch_key(device));

    if (dev->cfg.mode != MIRROR_MODE_DEPTH || create_info_count == 0 ||
        ensure_mirror_module(dev) != VK_SUCCESS)
        return dev->vt.CreateGraphicsPipelines(device, pipeline_cache, create_info_count,
                                               create_infos, allocator, pipelines);

    VkGraphicsPipelineCreateInfo *patched_infos = malloc(create_info_count * sizeof *patched_infos);
    struct pipeline_patch *patches = calloc(create_info_count, sizeof *patches);
    if (!patched_infos || !patches) {
        free(patched_infos);
        free(patches);
        return dev->vt.CreateGraphicsPipelines(device, pipeline_cache, create_info_count,
                                               create_infos, allocator, pipelines);
    }

    memcpy(patched_infos, create_infos, create_info_count * sizeof *patched_infos);
    for (uint32_t i = 0; i < create_info_count; i++)
        patch_pipeline(dev, &patched_infos[i], &patches[i]);

    VkResult result = dev->vt.CreateGraphicsPipelines(device, pipeline_cache, create_info_count,
                                                      patched_infos, allocator, pipelines);

    for (uint32_t i = 0; i < create_info_count; i++)
        free(patches[i].stages);
    free(patches);
    free(patched_infos);
    return result;
}

/* ------------------------------------------------------------ enumeration */

static VkResult fill_layer_properties(uint32_t *property_count, VkLayerProperties *properties) {
    if (!properties) {
        *property_count = 1;
        return VK_SUCCESS;
    }
    if (*property_count < 1)
        return VK_INCOMPLETE;
    *property_count = 1;
    *properties = (VkLayerProperties){
        .specVersion = MIRROR_SPEC_VERSION,
        .implementationVersion = MIRROR_IMPL_VERSION,
    };
    strcpy(properties->layerName, MIRROR_LAYER_NAME);
    strcpy(properties->description, MIRROR_LAYER_DESCRIPTION);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
mirror_EnumerateInstanceLayerProperties(uint32_t *property_count, VkLayerProperties *properties) {
    return fill_layer_properties(property_count, properties);
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physical_device, uint32_t *property_count, VkLayerProperties *properties) {
    (void)physical_device;
    return fill_layer_properties(property_count, properties);
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_EnumerateInstanceExtensionProperties(
    const char *layer_name, uint32_t *property_count, VkExtensionProperties *properties) {
    (void)properties;
    if (!layer_name || strcmp(layer_name, MIRROR_LAYER_NAME) != 0)
        return VK_ERROR_LAYER_NOT_PRESENT;
    *property_count = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mirror_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char *layer_name, uint32_t *property_count,
    VkExtensionProperties *properties) {
    if (layer_name && strcmp(layer_name, MIRROR_LAYER_NAME) == 0) {
        *property_count = 0;
        return VK_SUCCESS;
    }
    struct instance_data *inst = instance_find(dispatch_key(physical_device));
    if (!inst)
        return VK_ERROR_INITIALIZATION_FAILED;
    return inst->EnumerateDeviceExtensionProperties(physical_device, layer_name, property_count,
                                                    properties);
}

/* ------------------------------------------------------------- entrypoints */

MIRROR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                             const char *name);
MIRROR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                           const char *name);

#define MIRROR_INTERCEPT(fn)                                                                       \
    do {                                                                                           \
        if (strcmp(name, "vk" #fn) == 0)                                                           \
            return (PFN_vkVoidFunction)mirror_##fn;                                                \
    } while (0)

static PFN_vkVoidFunction intercepted_device_function(const char *name) {
    MIRROR_INTERCEPT(DestroyDevice);
    MIRROR_INTERCEPT(CreateShaderModule);
    MIRROR_INTERCEPT(DestroyShaderModule);
    MIRROR_INTERCEPT(CreateGraphicsPipelines);
    return NULL;
}

MIRROR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                             const char *name) {
    call_once(&g_once, global_init);

    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    MIRROR_INTERCEPT(CreateInstance);
    MIRROR_INTERCEPT(DestroyInstance);
    MIRROR_INTERCEPT(CreateDevice);
    MIRROR_INTERCEPT(EnumerateInstanceLayerProperties);
    MIRROR_INTERCEPT(EnumerateDeviceLayerProperties);
    MIRROR_INTERCEPT(EnumerateInstanceExtensionProperties);
    MIRROR_INTERCEPT(EnumerateDeviceExtensionProperties);

    PFN_vkVoidFunction device_function = intercepted_device_function(name);
    if (device_function)
        return device_function;

    if (!instance)
        return NULL;
    struct instance_data *inst = instance_find(dispatch_key(instance));
    if (!inst)
        return NULL;
    return inst->GetInstanceProcAddr(instance, name);
}

MIRROR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                           const char *name) {
    call_once(&g_once, global_init);

    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction device_function = intercepted_device_function(name);
    if (device_function)
        return device_function;

    if (!device)
        return NULL;
    struct device_data *dev = device_find(dispatch_key(device));
    if (!dev)
        return NULL;
    return dev->vt.GetDeviceProcAddr(device, name);
}
