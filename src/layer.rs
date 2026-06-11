//! The mirror Vulkan layer.
//!
//! `vkCreateShaderModule` is intercepted to remember the FNV-1a hash of each
//! module's SPIR-V, so fragment shaders ("materials") can be identified.
//! `vkCreateGraphicsPipelines` is intercepted to swap the application's
//! fragment shader for a tiny replacement that renders window-space depth as
//! grayscale, or a highlight color selected through specialization constants.
//!
//! The layer never wraps Vulkan handles; it only keeps side tables keyed by
//! the loader's dispatch keys, which keeps it transparent to applications and
//! to other layers.

use crate::config::{Config, Mode};
use crate::hash::fnv1a64;
use crate::include_spv;
use crate::vk_layer::{
    DeviceCreateInfo as LayerDeviceCreateInfo, InstanceCreateInfo as LayerInstanceCreateInfo,
    LINK_INFO, LOADER_DEVICE_CREATE_INFO, LOADER_INSTANCE_CREATE_INFO,
};
use ash::vk::{self, Handle};
use std::collections::HashMap;
use std::ffi::{c_char, CStr};
use std::ptr;
use std::sync::{Arc, Mutex, OnceLock};

const LAYER_NAME: &CStr = c"VK_LAYER_MIRROR_mirror";
const LAYER_DESCRIPTION: &CStr =
    c"Renders depth instead of materials, with configurable highlights";
const SPEC_VERSION: u32 = vk::make_api_version(0, 1, 3, 0);
const IMPL_VERSION: u32 = 1;

/// Per-instance state: just the down-chain functions the layer needs.
struct InstanceData {
    instance: vk::Instance,
    get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
    destroy_instance: vk::PFN_vkDestroyInstance,
    enumerate_device_extension_properties: vk::PFN_vkEnumerateDeviceExtensionProperties,
}

/// The down-chain device functions the layer dispatches to.
struct DeviceDispatch {
    get_device_proc_addr: vk::PFN_vkGetDeviceProcAddr,
    destroy_device: vk::PFN_vkDestroyDevice,
    create_shader_module: vk::PFN_vkCreateShaderModule,
    destroy_shader_module: vk::PFN_vkDestroyShaderModule,
    create_graphics_pipelines: vk::PFN_vkCreateGraphicsPipelines,
}

/// Mutable per-device state, guarded by a lock.
struct DeviceState {
    /// `VkShaderModule` handle -> SPIR-V hash.
    module_hashes: HashMap<u64, u64>,
    /// Lazily created replacement shader (`null` until first needed).
    mirror_module: vk::ShaderModule,
}

struct DeviceData {
    dispatch: DeviceDispatch,
    config: Config,
    state: Mutex<DeviceState>,
}

// The layer guarantees external synchronization through the global registry
// lock and per-device state lock; the raw handles and function pointers it
// stores are themselves plain data.
unsafe impl Send for InstanceData {}
unsafe impl Sync for InstanceData {}
unsafe impl Send for DeviceData {}
unsafe impl Sync for DeviceData {}

#[derive(Default)]
struct Registry {
    instances: HashMap<usize, Arc<InstanceData>>,
    devices: HashMap<usize, Arc<DeviceData>>,
}

fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Registry::default()))
}

fn find_instance(key: usize) -> Option<Arc<InstanceData>> {
    registry().lock().unwrap().instances.get(&key).cloned()
}

fn find_device(key: usize) -> Option<Arc<DeviceData>> {
    registry().lock().unwrap().devices.get(&key).cloned()
}

/// The loader writes a dispatch-table pointer into the first word of every
/// dispatchable handle; that pointer uniquely identifies the instance/device
/// chain and is what the layer keys its side tables on.
unsafe fn dispatch_key(handle: u64) -> usize {
    *(handle as usize as *const usize)
}

/// Reinterprets a typed Vulkan function pointer as the loader's opaque
/// `PFN_vkVoidFunction`. The argument is always a `system` fn pointer.
fn as_void<T>(function: T) -> vk::PFN_vkVoidFunction {
    // SAFETY: every caller passes a non-null `extern "system"` fn pointer,
    // which is pointer-sized and matches the `PFN_vkVoidFunction` niche.
    unsafe { std::mem::transmute_copy(&function) }
}

/// Loads a typed instance-level function pointer through `gipa`.
unsafe fn load_instance<T>(
    gipa: vk::PFN_vkGetInstanceProcAddr,
    instance: vk::Instance,
    name: &CStr,
) -> Option<T> {
    let function = gipa(instance, name.as_ptr())?;
    Some(std::mem::transmute_copy(&function))
}

/// Loads a typed device-level function pointer through `gdpa`.
unsafe fn load_device<T>(
    gdpa: vk::PFN_vkGetDeviceProcAddr,
    device: vk::Device,
    name: &CStr,
) -> Option<T> {
    let function = gdpa(device, name.as_ptr())?;
    Some(std::mem::transmute_copy(&function))
}

fn log(config: &Config, args: std::fmt::Arguments) {
    if config.log {
        eprintln!("[mirror] {args}");
    }
}

/* ---------------------------------------------------------------- instance */

unsafe fn find_instance_link(
    create_info: *const vk::InstanceCreateInfo,
) -> Option<*mut LayerInstanceCreateInfo> {
    let mut node = (*create_info).p_next as *const vk::BaseInStructure;
    while !node.is_null() {
        if (*node).s_type == LOADER_INSTANCE_CREATE_INFO {
            let link = node as *mut LayerInstanceCreateInfo;
            if (*link).function == LINK_INFO {
                return Some(link);
            }
        }
        node = (*node).p_next;
    }
    None
}

unsafe extern "system" fn create_instance(
    p_create_info: *const vk::InstanceCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_instance: *mut vk::Instance,
) -> vk::Result {
    let Some(chain) = find_instance_link(p_create_info) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let link = (*chain).layer_info;
    if link.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let next_gipa = (*link).get_instance_proc_addr;
    let Some(next_create) = load_instance::<vk::PFN_vkCreateInstance>(
        next_gipa,
        vk::Instance::null(),
        c"vkCreateInstance",
    ) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };

    // Advance the chain for the layers below us.
    (*chain).layer_info = (*link).next;

    let result = next_create(p_create_info, p_allocator, p_instance);
    if result != vk::Result::SUCCESS {
        return result;
    }
    let instance = *p_instance;

    let data = InstanceData {
        instance,
        get_instance_proc_addr: next_gipa,
        destroy_instance: load_instance(next_gipa, instance, c"vkDestroyInstance").unwrap(),
        enumerate_device_extension_properties: load_instance(
            next_gipa,
            instance,
            c"vkEnumerateDeviceExtensionProperties",
        )
        .unwrap(),
    };
    registry()
        .lock()
        .unwrap()
        .instances
        .insert(dispatch_key(instance.as_raw()), Arc::new(data));
    vk::Result::SUCCESS
}

unsafe extern "system" fn destroy_instance(
    instance: vk::Instance,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if instance == vk::Instance::null() {
        return;
    }
    let key = dispatch_key(instance.as_raw());
    let Some(data) = registry().lock().unwrap().instances.remove(&key) else {
        return;
    };
    (data.destroy_instance)(instance, p_allocator);
}

/* ------------------------------------------------------------------ device */

unsafe fn find_device_link(
    create_info: *const vk::DeviceCreateInfo,
) -> Option<*mut LayerDeviceCreateInfo> {
    let mut node = (*create_info).p_next as *const vk::BaseInStructure;
    while !node.is_null() {
        if (*node).s_type == LOADER_DEVICE_CREATE_INFO {
            let link = node as *mut LayerDeviceCreateInfo;
            if (*link).function == LINK_INFO {
                return Some(link);
            }
        }
        node = (*node).p_next;
    }
    None
}

unsafe extern "system" fn create_device(
    physical_device: vk::PhysicalDevice,
    p_create_info: *const vk::DeviceCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_device: *mut vk::Device,
) -> vk::Result {
    let Some(chain) = find_device_link(p_create_info) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    let link = (*chain).layer_info;
    if link.is_null() {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    }
    let next_gipa = (*link).get_instance_proc_addr;
    let next_gdpa = (*link).get_device_proc_addr;

    let instance = find_instance(dispatch_key(physical_device.as_raw()))
        .map_or(vk::Instance::null(), |inst| inst.instance);
    let Some(next_create) =
        load_instance::<vk::PFN_vkCreateDevice>(next_gipa, instance, c"vkCreateDevice")
    else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };

    (*chain).layer_info = (*link).next;

    let result = next_create(physical_device, p_create_info, p_allocator, p_device);
    if result != vk::Result::SUCCESS {
        return result;
    }
    let device = *p_device;

    let dispatch = DeviceDispatch {
        get_device_proc_addr: next_gdpa,
        destroy_device: load_device(next_gdpa, device, c"vkDestroyDevice").unwrap(),
        create_shader_module: load_device(next_gdpa, device, c"vkCreateShaderModule").unwrap(),
        destroy_shader_module: load_device(next_gdpa, device, c"vkDestroyShaderModule").unwrap(),
        create_graphics_pipelines: load_device(next_gdpa, device, c"vkCreateGraphicsPipelines")
            .unwrap(),
    };
    let config = Config::from_env();
    log(
        &config,
        format_args!(
            "device created, mode={}, {} highlight(s)",
            if config.mode == Mode::Depth {
                "depth"
            } else {
                "off"
            },
            config.highlights.len(),
        ),
    );

    let data = DeviceData {
        dispatch,
        config,
        state: Mutex::new(DeviceState {
            module_hashes: HashMap::new(),
            mirror_module: vk::ShaderModule::null(),
        }),
    };
    registry()
        .lock()
        .unwrap()
        .devices
        .insert(dispatch_key(device.as_raw()), Arc::new(data));
    vk::Result::SUCCESS
}

unsafe extern "system" fn destroy_device(
    device: vk::Device,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    if device == vk::Device::null() {
        return;
    }
    let key = dispatch_key(device.as_raw());
    let Some(data) = registry().lock().unwrap().devices.remove(&key) else {
        return;
    };
    let mirror_module = data.state.lock().unwrap().mirror_module;
    if mirror_module != vk::ShaderModule::null() {
        (data.dispatch.destroy_shader_module)(device, mirror_module, ptr::null());
    }
    (data.dispatch.destroy_device)(device, p_allocator);
}

/* --------------------------------------------------------- shader tracking */

unsafe extern "system" fn create_shader_module(
    device: vk::Device,
    p_create_info: *const vk::ShaderModuleCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_shader_module: *mut vk::ShaderModule,
) -> vk::Result {
    let dev = find_device(dispatch_key(device.as_raw())).unwrap();
    let result =
        (dev.dispatch.create_shader_module)(device, p_create_info, p_allocator, p_shader_module);
    if result != vk::Result::SUCCESS {
        return result;
    }
    let info = &*p_create_info;
    let code = std::slice::from_raw_parts(info.p_code.cast::<u8>(), info.code_size);
    let hash = fnv1a64(code);
    let module = *p_shader_module;
    dev.state
        .lock()
        .unwrap()
        .module_hashes
        .insert(module.as_raw(), hash);
    log(
        &dev.config,
        format_args!(
            "shader module created, hash={hash:016x} ({} bytes)",
            info.code_size
        ),
    );
    vk::Result::SUCCESS
}

unsafe extern "system" fn destroy_shader_module(
    device: vk::Device,
    shader_module: vk::ShaderModule,
    p_allocator: *const vk::AllocationCallbacks<'_>,
) {
    let dev = find_device(dispatch_key(device.as_raw())).unwrap();
    if shader_module != vk::ShaderModule::null() {
        dev.state
            .lock()
            .unwrap()
            .module_hashes
            .remove(&shader_module.as_raw());
    }
    (dev.dispatch.destroy_shader_module)(device, shader_module, p_allocator);
}

/* ------------------------------------------------------ pipeline patching */

#[repr(C)]
struct SpecPayload {
    /// 0 = depth grayscale, 1 = highlight color.
    mode: u32,
    color: [f32; 3],
}

const fn spec_entry(constant_id: u32, offset: u32) -> vk::SpecializationMapEntry {
    vk::SpecializationMapEntry {
        constant_id,
        offset,
        size: std::mem::size_of::<f32>(),
    }
}

const SPEC_ENTRIES: [vk::SpecializationMapEntry; 4] = [
    spec_entry(0, 0),
    spec_entry(1, 4),
    spec_entry(2, 8),
    spec_entry(3, 12),
];

/// Owns the heap data a patched fragment stage points at; kept alive until
/// `vkCreateGraphicsPipelines` returns.
struct PipelinePatch {
    stages: Vec<vk::PipelineShaderStageCreateInfo<'static>>,
    _spec_info: Box<vk::SpecializationInfo<'static>>,
    _payload: Box<SpecPayload>,
    _entries: Box<[vk::SpecializationMapEntry; 4]>,
}

unsafe fn ensure_mirror_module(
    dev: &DeviceData,
    device: vk::Device,
) -> Result<vk::ShaderModule, vk::Result> {
    let mut state = dev.state.lock().unwrap();
    if state.mirror_module == vk::ShaderModule::null() {
        let spv = include_spv!("mirror.frag");
        let info = vk::ShaderModuleCreateInfo {
            code_size: spv.bytes().len(),
            p_code: spv.words().as_ptr(),
            ..Default::default()
        };
        let mut module = vk::ShaderModule::null();
        let result = (dev.dispatch.create_shader_module)(device, &info, ptr::null(), &mut module);
        if result != vk::Result::SUCCESS {
            return Err(result);
        }
        state.mirror_module = module;
    }
    Ok(state.mirror_module)
}

/// Determines the SPIR-V hash of a fragment stage, either through the module
/// registry or from an inline `VkShaderModuleCreateInfo` in its `pNext` chain
/// (`VK_KHR_maintenance5` style).
unsafe fn fragment_stage_hash(
    dev: &DeviceData,
    stage: &vk::PipelineShaderStageCreateInfo,
) -> Option<u64> {
    if stage.module != vk::ShaderModule::null() {
        return dev
            .state
            .lock()
            .unwrap()
            .module_hashes
            .get(&stage.module.as_raw())
            .copied();
    }
    let mut node = stage.p_next as *const vk::BaseInStructure;
    while !node.is_null() {
        if (*node).s_type == vk::StructureType::SHADER_MODULE_CREATE_INFO {
            let inline = node as *const vk::ShaderModuleCreateInfo;
            let code =
                std::slice::from_raw_parts((*inline).p_code.cast::<u8>(), (*inline).code_size);
            return Some(fnv1a64(code));
        }
        node = (*node).p_next;
    }
    None
}

unsafe fn build_patch(
    dev: &DeviceData,
    mirror_module: vk::ShaderModule,
    info: &vk::GraphicsPipelineCreateInfo,
) -> Option<PipelinePatch> {
    if info.p_stages.is_null() || info.stage_count == 0 {
        return None;
    }
    let original = std::slice::from_raw_parts(info.p_stages, info.stage_count as usize);
    // The last fragment stage wins, matching the C reference.
    let fragment_index = original
        .iter()
        .rposition(|s| s.stage == vk::ShaderStageFlags::FRAGMENT)?;

    let hash = fragment_stage_hash(dev, &original[fragment_index]);
    let highlight = hash.and_then(|hash| dev.config.highlight(hash)).copied();

    let payload = Box::new(match highlight {
        Some(highlight) => SpecPayload {
            mode: 1,
            color: highlight.color,
        },
        None => SpecPayload {
            mode: 0,
            color: [0.0; 3],
        },
    });
    let entries = Box::new(SPEC_ENTRIES);
    let spec_info = Box::new(vk::SpecializationInfo {
        map_entry_count: entries.len() as u32,
        p_map_entries: entries.as_ptr(),
        data_size: std::mem::size_of::<SpecPayload>(),
        p_data: ptr::from_ref(&*payload).cast(),
        ..Default::default()
    });

    // The pointers each copied stage holds (module handle, entry-point name,
    // app-owned pNext) all outlive the create call we forward them to, so the
    // borrow-tracking lifetime can be safely erased to 'static.
    let mut stages: Vec<vk::PipelineShaderStageCreateInfo<'static>> = original
        .iter()
        .map(|&stage| std::mem::transmute(stage))
        .collect();
    // The original stage's pNext described the shader being replaced, so it is
    // intentionally dropped along with the original specialization data.
    stages[fragment_index] = vk::PipelineShaderStageCreateInfo {
        stage: vk::ShaderStageFlags::FRAGMENT,
        module: mirror_module,
        p_name: c"main".as_ptr(),
        p_specialization_info: ptr::from_ref(&*spec_info),
        ..Default::default()
    };

    match hash {
        Some(hash) => log(
            &dev.config,
            format_args!(
                "pipeline patched, fragment hash={hash:016x}{}",
                if highlight.is_some() {
                    " (highlighted)"
                } else {
                    ""
                },
            ),
        ),
        None => log(
            &dev.config,
            format_args!("pipeline patched, fragment shader unidentified"),
        ),
    }

    Some(PipelinePatch {
        stages,
        _spec_info: spec_info,
        _payload: payload,
        _entries: entries,
    })
}

unsafe extern "system" fn create_graphics_pipelines(
    device: vk::Device,
    pipeline_cache: vk::PipelineCache,
    create_info_count: u32,
    p_create_infos: *const vk::GraphicsPipelineCreateInfo<'_>,
    p_allocator: *const vk::AllocationCallbacks<'_>,
    p_pipelines: *mut vk::Pipeline,
) -> vk::Result {
    let dev = find_device(dispatch_key(device.as_raw())).unwrap();

    let passthrough = || {
        (dev.dispatch.create_graphics_pipelines)(
            device,
            pipeline_cache,
            create_info_count,
            p_create_infos,
            p_allocator,
            p_pipelines,
        )
    };
    if dev.config.mode != Mode::Depth || create_info_count == 0 {
        return passthrough();
    }
    let mirror_module = match ensure_mirror_module(&dev, device) {
        Ok(module) => module,
        Err(_) => return passthrough(),
    };

    let mut patched =
        std::slice::from_raw_parts(p_create_infos, create_info_count as usize).to_vec();
    // Keep the per-pipeline patch data alive until the create call returns.
    let mut patches = Vec::with_capacity(patched.len());
    for info in &mut patched {
        if let Some(patch) = build_patch(&dev, mirror_module, info) {
            info.stage_count = patch.stages.len() as u32;
            info.p_stages = patch.stages.as_ptr();
            patches.push(patch);
        }
    }

    (dev.dispatch.create_graphics_pipelines)(
        device,
        pipeline_cache,
        create_info_count,
        patched.as_ptr(),
        p_allocator,
        p_pipelines,
    )
}

/* ------------------------------------------------------------ enumeration */

unsafe fn write_layer_string(dst: &mut [c_char], src: &CStr) {
    for (slot, &byte) in dst.iter_mut().zip(src.to_bytes_with_nul()) {
        *slot = byte as c_char;
    }
}

unsafe fn fill_layer_properties(
    property_count: *mut u32,
    properties: *mut vk::LayerProperties,
) -> vk::Result {
    if properties.is_null() {
        *property_count = 1;
        return vk::Result::SUCCESS;
    }
    if *property_count < 1 {
        return vk::Result::INCOMPLETE;
    }
    *property_count = 1;
    let mut props = vk::LayerProperties {
        spec_version: SPEC_VERSION,
        implementation_version: IMPL_VERSION,
        ..Default::default()
    };
    write_layer_string(&mut props.layer_name, LAYER_NAME);
    write_layer_string(&mut props.description, LAYER_DESCRIPTION);
    *properties = props;
    vk::Result::SUCCESS
}

unsafe extern "system" fn enumerate_instance_layer_properties(
    property_count: *mut u32,
    properties: *mut vk::LayerProperties,
) -> vk::Result {
    fill_layer_properties(property_count, properties)
}

unsafe extern "system" fn enumerate_device_layer_properties(
    _physical_device: vk::PhysicalDevice,
    property_count: *mut u32,
    properties: *mut vk::LayerProperties,
) -> vk::Result {
    fill_layer_properties(property_count, properties)
}

unsafe extern "system" fn enumerate_instance_extension_properties(
    p_layer_name: *const c_char,
    property_count: *mut u32,
    _properties: *mut vk::ExtensionProperties,
) -> vk::Result {
    if p_layer_name.is_null() || CStr::from_ptr(p_layer_name) != LAYER_NAME {
        return vk::Result::ERROR_LAYER_NOT_PRESENT;
    }
    *property_count = 0;
    vk::Result::SUCCESS
}

unsafe extern "system" fn enumerate_device_extension_properties(
    physical_device: vk::PhysicalDevice,
    p_layer_name: *const c_char,
    property_count: *mut u32,
    properties: *mut vk::ExtensionProperties,
) -> vk::Result {
    if !p_layer_name.is_null() && CStr::from_ptr(p_layer_name) == LAYER_NAME {
        *property_count = 0;
        return vk::Result::SUCCESS;
    }
    let Some(inst) = find_instance(dispatch_key(physical_device.as_raw())) else {
        return vk::Result::ERROR_INITIALIZATION_FAILED;
    };
    (inst.enumerate_device_extension_properties)(
        physical_device,
        p_layer_name,
        property_count,
        properties,
    )
}

/* ------------------------------------------------------------- entrypoints */

fn intercept_instance(name: &CStr) -> Option<vk::PFN_vkVoidFunction> {
    Some(match name.to_bytes() {
        b"vkCreateInstance" => as_void(create_instance as vk::PFN_vkCreateInstance),
        b"vkDestroyInstance" => as_void(destroy_instance as vk::PFN_vkDestroyInstance),
        b"vkCreateDevice" => as_void(create_device as vk::PFN_vkCreateDevice),
        b"vkEnumerateInstanceLayerProperties" => as_void(
            enumerate_instance_layer_properties as vk::PFN_vkEnumerateInstanceLayerProperties,
        ),
        b"vkEnumerateDeviceLayerProperties" => {
            as_void(enumerate_device_layer_properties as vk::PFN_vkEnumerateDeviceLayerProperties)
        }
        b"vkEnumerateInstanceExtensionProperties" => as_void(
            enumerate_instance_extension_properties
                as vk::PFN_vkEnumerateInstanceExtensionProperties,
        ),
        b"vkEnumerateDeviceExtensionProperties" => as_void(
            enumerate_device_extension_properties as vk::PFN_vkEnumerateDeviceExtensionProperties,
        ),
        _ => return None,
    })
}

fn intercept_device(name: &CStr) -> Option<vk::PFN_vkVoidFunction> {
    Some(match name.to_bytes() {
        b"vkDestroyDevice" => as_void(destroy_device as vk::PFN_vkDestroyDevice),
        b"vkCreateShaderModule" => as_void(create_shader_module as vk::PFN_vkCreateShaderModule),
        b"vkDestroyShaderModule" => as_void(destroy_shader_module as vk::PFN_vkDestroyShaderModule),
        b"vkCreateGraphicsPipelines" => {
            as_void(create_graphics_pipelines as vk::PFN_vkCreateGraphicsPipelines)
        }
        _ => return None,
    })
}

/// The layer's instance proc-addr entrypoint, exported for the Vulkan loader.
///
/// # Safety
/// Called by the Vulkan loader with a valid instance (or null) and a
/// NUL-terminated name, per the loader/layer interface.
#[no_mangle]
pub unsafe extern "system" fn vkGetInstanceProcAddr(
    instance: vk::Instance,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    let name = CStr::from_ptr(p_name);
    if name == c"vkGetInstanceProcAddr" {
        return as_void(vkGetInstanceProcAddr as vk::PFN_vkGetInstanceProcAddr);
    }
    if name == c"vkGetDeviceProcAddr" {
        return as_void(vkGetDeviceProcAddr as vk::PFN_vkGetDeviceProcAddr);
    }
    if let Some(function) = intercept_instance(name).or_else(|| intercept_device(name)) {
        return function;
    }
    if instance == vk::Instance::null() {
        return None;
    }
    let inst = find_instance(dispatch_key(instance.as_raw()))?;
    (inst.get_instance_proc_addr)(instance, p_name)
}

/// The layer's device proc-addr entrypoint, exported for the Vulkan loader.
///
/// # Safety
/// Called by the Vulkan loader with a valid device (or null) and a
/// NUL-terminated name, per the loader/layer interface.
#[no_mangle]
pub unsafe extern "system" fn vkGetDeviceProcAddr(
    device: vk::Device,
    p_name: *const c_char,
) -> vk::PFN_vkVoidFunction {
    let name = CStr::from_ptr(p_name);
    if name == c"vkGetDeviceProcAddr" {
        return as_void(vkGetDeviceProcAddr as vk::PFN_vkGetDeviceProcAddr);
    }
    if let Some(function) = intercept_device(name) {
        return function;
    }
    if device == vk::Device::null() {
        return None;
    }
    let dev = find_device(dispatch_key(device.as_raw()))?;
    (dev.dispatch.get_device_proc_addr)(device, p_name)
}
