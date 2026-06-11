//! The Vulkan loader/layer interface structs, which `ash` does not expose.
//!
//! When the loader creates an instance or device it threads a linked list of
//! these structures through the `pNext` chain of the create info. Each layer
//! finds its link, grabs the next layer's `vkGet*ProcAddr`, and advances the
//! list before calling down. Layouts here mirror `<vulkan/vk_layer.h>`; only
//! the fields the layer actually reads are modeled (the create-info `u` field
//! is a union whose first member is the link pointer).

use ash::vk;
use std::ffi::c_void;

/// `VkLayerFunction::VK_LAYER_LINK_INFO`.
pub const LINK_INFO: i32 = 0;

/// `VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO`.
pub const LOADER_INSTANCE_CREATE_INFO: vk::StructureType = vk::StructureType::from_raw(47);
/// `VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO`.
pub const LOADER_DEVICE_CREATE_INFO: vk::StructureType = vk::StructureType::from_raw(48);

/// `VkLayerInstanceLink`: the next instance layer's proc-addr getters.
#[repr(C)]
pub struct InstanceLink {
    pub next: *mut InstanceLink,
    pub get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
    pub get_physical_device_proc_addr: Option<unsafe extern "system" fn()>,
}

/// `VkLayerInstanceCreateInfo` (only up to the link-pointer union member).
#[repr(C)]
pub struct InstanceCreateInfo {
    pub s_type: vk::StructureType,
    pub next: *const c_void,
    pub function: i32,
    pub layer_info: *mut InstanceLink,
}

/// `VkLayerDeviceLink`: the next device layer's proc-addr getters.
#[repr(C)]
pub struct DeviceLink {
    pub next: *mut DeviceLink,
    pub get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
    pub get_device_proc_addr: vk::PFN_vkGetDeviceProcAddr,
}

/// `VkLayerDeviceCreateInfo` (only up to the link-pointer union member).
#[repr(C)]
pub struct DeviceCreateInfo {
    pub s_type: vk::StructureType,
    pub next: *const c_void,
    pub function: i32,
    pub layer_info: *mut DeviceLink,
}
