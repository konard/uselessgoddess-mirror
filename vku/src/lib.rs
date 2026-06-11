//! A minimal headless Vulkan offscreen-rendering harness shared by the mirror
//! layer's integration tests and its `scene` example.
//!
//! It works on any Vulkan 1.1 implementation, including CPU drivers such as
//! lavapipe, so everything runs without a display in CI. [`Ctx`] owns an
//! instance, device and queue; [`Ctx::render`] rasterizes a list of [`Draw`]
//! rectangles — each a fullscreen triangle clipped by viewport/scissor and
//! placed at an exact depth — into an RGBA8 image and reads it back.

mod layer;

pub use layer::{install as install_layer, MIRROR_LAYER_NAME, VALIDATION_LAYER_NAME};

use ash::vk;
use std::ffi::{c_void, CStr};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};

/// SPIR-V over-aligned for `VkShaderModuleCreateInfo::pCode` (4-byte aligned).
#[repr(C, align(4))]
struct Aligned<B: ?Sized>(B);

static FULLSCREEN_VERT: &Aligned<[u8]> = &Aligned(*include_bytes!(concat!(
    env!("OUT_DIR"),
    "/fullscreen.vert.spv"
)));

fn fullscreen_vert_words() -> &'static [u32] {
    let bytes = &FULLSCREEN_VERT.0;
    // SAFETY: `Aligned` forces 4-byte alignment and SPIR-V length is a whole
    // number of 32-bit words.
    unsafe { std::slice::from_raw_parts(bytes.as_ptr().cast::<u32>(), bytes.len() / 4) }
}

/// Why [`Ctx::init`] could not produce a usable context.
#[derive(Debug)]
pub enum InitError {
    /// No usable Vulkan implementation; the caller should skip (exit 77).
    NoDevice,
    /// The Vulkan loader could not be loaded at all.
    Loading,
    /// A Vulkan call failed unexpectedly.
    Vulkan(vk::Result),
}

impl std::fmt::Display for InitError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoDevice => write!(f, "no usable Vulkan device"),
            Self::Loading => write!(f, "failed to load the Vulkan loader"),
            Self::Vulkan(result) => write!(f, "Vulkan call failed: {result:?}"),
        }
    }
}

impl std::error::Error for InitError {}

/// One rectangle drawn with the given fragment shader at a fixed depth.
pub struct Draw {
    /// The fragment shader's SPIR-V words.
    pub frag_spv: &'static [u32],
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    /// Window-space depth in `[0, 1]`, applied through the viewport range.
    pub depth: f32,
}

/// An offscreen Vulkan context: instance, device and a graphics queue.
pub struct Ctx {
    // Kept alive so its loader/dispatch tables outlive `instance`/`device`.
    _entry: ash::Entry,
    instance: ash::Instance,
    device: ash::Device,
    physical_device: vk::PhysicalDevice,
    queue: vk::Queue,
    queue_family: u32,
    debug: Option<DebugMessenger>,
}

struct DebugMessenger {
    utils: ash::ext::debug_utils::Instance,
    messenger: vk::DebugUtilsMessengerEXT,
    // Boxed so the pointer handed to the callback stays valid as `Ctx` moves.
    errors: Box<AtomicU64>,
}

/// Returns whether the loader can see `layer_name` (for the `enumerate` test).
#[must_use]
pub fn layer_available(layer_name: &CStr) -> bool {
    let Ok(entry) = (unsafe { ash::Entry::load() }) else {
        return false;
    };
    let Ok(properties) = (unsafe { entry.enumerate_instance_layer_properties() }) else {
        return false;
    };
    properties.iter().any(|properties| {
        let name = unsafe { CStr::from_ptr(properties.layer_name.as_ptr()) };
        name == layer_name
    })
}

unsafe extern "system" fn debug_callback(
    severity: vk::DebugUtilsMessageSeverityFlagsEXT,
    _types: vk::DebugUtilsMessageTypeFlagsEXT,
    callback_data: *const vk::DebugUtilsMessengerCallbackDataEXT<'_>,
    user_data: *mut c_void,
) -> vk::Bool32 {
    if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR) {
        let errors = &*user_data.cast::<AtomicU64>();
        errors.fetch_add(1, Ordering::Relaxed);
        if !(*callback_data).p_message.is_null() {
            let message = CStr::from_ptr((*callback_data).p_message);
            eprintln!("[validation] {}", message.to_string_lossy());
        }
    }
    vk::FALSE
}

impl Ctx {
    /// Creates a context, optionally enabling the mirror layer and the Khronos
    /// validation layer (with an error-counting debug messenger).
    pub fn init(enable_mirror_layer: bool, enable_validation: bool) -> Result<Self, InitError> {
        if enable_mirror_layer && !layer::install() {
            return Err(InitError::NoDevice);
        }
        let entry = unsafe { ash::Entry::load() }.map_err(|_| InitError::Loading)?;

        let mut layers: Vec<*const i8> = Vec::new();
        if enable_mirror_layer {
            layers.push(MIRROR_LAYER_NAME.as_ptr());
        }
        if enable_validation {
            layers.push(VALIDATION_LAYER_NAME.as_ptr());
        }
        let mut extensions: Vec<*const i8> = Vec::new();
        if enable_validation {
            extensions.push(ash::ext::debug_utils::NAME.as_ptr());
        }

        let application_info = vk::ApplicationInfo::default()
            .application_name(c"mirror-vkutil")
            .api_version(vk::API_VERSION_1_1);
        let instance_info = vk::InstanceCreateInfo::default()
            .application_info(&application_info)
            .enabled_layer_names(&layers)
            .enabled_extension_names(&extensions);

        let instance = match unsafe { entry.create_instance(&instance_info, None) } {
            Ok(instance) => instance,
            Err(vk::Result::ERROR_INCOMPATIBLE_DRIVER) => return Err(InitError::NoDevice),
            Err(result) => return Err(InitError::Vulkan(result)),
        };

        // `ash::Instance` is a cheap handle; the clone refers to the same
        // `VkInstance`, so on failure we destroy it exactly once here.
        match Self::build(entry, instance.clone(), enable_validation) {
            Ok(ctx) => Ok(ctx),
            Err(error) => unsafe {
                instance.destroy_instance(None);
                Err(error)
            },
        }
    }

    fn build(
        entry: ash::Entry,
        instance: ash::Instance,
        enable_validation: bool,
    ) -> Result<Self, InitError> {
        let debug = if enable_validation {
            let errors = Box::new(AtomicU64::new(0));
            let utils = ash::ext::debug_utils::Instance::new(&entry, &instance);
            let info = vk::DebugUtilsMessengerCreateInfoEXT::default()
                .message_severity(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR)
                .message_type(
                    vk::DebugUtilsMessageTypeFlagsEXT::VALIDATION
                        | vk::DebugUtilsMessageTypeFlagsEXT::GENERAL,
                )
                .pfn_user_callback(Some(debug_callback))
                .user_data(std::ptr::from_ref(&*errors) as *mut c_void);
            let messenger = unsafe { utils.create_debug_utils_messenger(&info, None) }
                .map_err(InitError::Vulkan)?;
            Some(DebugMessenger {
                utils,
                messenger,
                errors,
            })
        } else {
            None
        };

        let physical_devices =
            unsafe { instance.enumerate_physical_devices() }.map_err(InitError::Vulkan)?;
        let (physical_device, queue_family) = physical_devices
            .into_iter()
            .find_map(|device| {
                let families =
                    unsafe { instance.get_physical_device_queue_family_properties(device) };
                families
                    .iter()
                    .position(|family| family.queue_flags.contains(vk::QueueFlags::GRAPHICS))
                    .map(|family| (device, family as u32))
            })
            .ok_or(InitError::NoDevice)?;

        let priorities = [1.0_f32];
        let queue_info = vk::DeviceQueueCreateInfo::default()
            .queue_family_index(queue_family)
            .queue_priorities(&priorities);
        let queue_infos = [queue_info];
        let device_info = vk::DeviceCreateInfo::default().queue_create_infos(&queue_infos);
        let device = unsafe { instance.create_device(physical_device, &device_info, None) }
            .map_err(InitError::Vulkan)?;
        let queue = unsafe { device.get_device_queue(queue_family, 0) };

        Ok(Self {
            _entry: entry,
            instance,
            device,
            physical_device,
            queue,
            queue_family,
            debug,
        })
    }

    /// The number of validation errors seen so far (always `0` without
    /// validation enabled).
    #[must_use]
    pub fn validation_errors(&self) -> u64 {
        self.debug
            .as_ref()
            .map_or(0, |debug| debug.errors.load(Ordering::Relaxed))
    }

    /// Renders `draws` into a `width`×`height` RGBA8 image, returned row-major.
    pub fn render(&self, width: u32, height: u32, draws: &[Draw]) -> Result<Vec<u8>, vk::Result> {
        unsafe { self.render_inner(width, height, draws) }
    }

    unsafe fn render_inner(
        &self,
        width: u32,
        height: u32,
        draws: &[Draw],
    ) -> Result<Vec<u8>, vk::Result> {
        let device = &self.device;
        let format = vk::Format::R8G8B8A8_UNORM;
        let byte_size = u64::from(width) * u64::from(height) * 4;
        let memory_properties = self
            .instance
            .get_physical_device_memory_properties(self.physical_device);

        // Color target.
        let image_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(format)
            .extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::TRANSFER_SRC)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        let image = device.create_image(&image_info, None)?;

        let image_requirements = device.get_image_memory_requirements(image);
        let image_memory_type = find_memory_type(
            &memory_properties,
            image_requirements.memory_type_bits,
            vk::MemoryPropertyFlags::empty(),
        )
        .ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)?;
        let image_alloc = vk::MemoryAllocateInfo::default()
            .allocation_size(image_requirements.size)
            .memory_type_index(image_memory_type);
        let image_memory = device.allocate_memory(&image_alloc, None)?;
        device.bind_image_memory(image, image_memory, 0)?;

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(format)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            });
        let view = device.create_image_view(&view_info, None)?;

        // Render pass: clear to black, end in TRANSFER_SRC for the readback.
        let attachment = vk::AttachmentDescription::default()
            .format(format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL);
        let attachments = [attachment];
        let color_reference = vk::AttachmentReference::default()
            .attachment(0)
            .layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL);
        let color_references = [color_reference];
        let subpass = vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_references);
        let subpasses = [subpass];
        let dependency = vk::SubpassDependency::default()
            .src_subpass(0)
            .dst_subpass(vk::SUBPASS_EXTERNAL)
            .src_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
            .dst_stage_mask(vk::PipelineStageFlags::TRANSFER)
            .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
            .dst_access_mask(vk::AccessFlags::TRANSFER_READ);
        let dependencies = [dependency];
        let render_pass_info = vk::RenderPassCreateInfo::default()
            .attachments(&attachments)
            .subpasses(&subpasses)
            .dependencies(&dependencies);
        let render_pass = device.create_render_pass(&render_pass_info, None)?;

        let framebuffer_attachments = [view];
        let framebuffer_info = vk::FramebufferCreateInfo::default()
            .render_pass(render_pass)
            .attachments(&framebuffer_attachments)
            .width(width)
            .height(height)
            .layers(1);
        let framebuffer = device.create_framebuffer(&framebuffer_info, None)?;

        // Host-visible readback buffer.
        let buffer_info = vk::BufferCreateInfo::default()
            .size(byte_size)
            .usage(vk::BufferUsageFlags::TRANSFER_DST);
        let readback = device.create_buffer(&buffer_info, None)?;
        let buffer_requirements = device.get_buffer_memory_requirements(readback);
        let buffer_memory_type = find_memory_type(
            &memory_properties,
            buffer_requirements.memory_type_bits,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
        )
        .ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)?;
        let buffer_alloc = vk::MemoryAllocateInfo::default()
            .allocation_size(buffer_requirements.size)
            .memory_type_index(buffer_memory_type);
        let readback_memory = device.allocate_memory(&buffer_alloc, None)?;
        device.bind_buffer_memory(readback, readback_memory, 0)?;

        // One pipeline per draw, created in a single call to exercise batch
        // patching in the layer.
        let vertex_info = vk::ShaderModuleCreateInfo::default().code(fullscreen_vert_words());
        let vertex_module = device.create_shader_module(&vertex_info, None)?;

        let mut fragment_modules = Vec::with_capacity(draws.len());
        for draw in draws {
            let info = vk::ShaderModuleCreateInfo::default().code(draw.frag_spv);
            fragment_modules.push(device.create_shader_module(&info, None)?);
        }

        let layout_info = vk::PipelineLayoutCreateInfo::default();
        let layout = device.create_pipeline_layout(&layout_info, None)?;

        let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();
        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
        let viewport_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);
        let rasterization = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
            .line_width(1.0);
        let multisample = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);
        let blend_attachment = vk::PipelineColorBlendAttachmentState::default().color_write_mask(
            vk::ColorComponentFlags::R
                | vk::ColorComponentFlags::G
                | vk::ColorComponentFlags::B
                | vk::ColorComponentFlags::A,
        );
        let blend_attachments = [blend_attachment];
        let color_blend =
            vk::PipelineColorBlendStateCreateInfo::default().attachments(&blend_attachments);
        let dynamic_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic_state =
            vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic_states);

        let entry_point = c"main";
        let stages: Vec<[vk::PipelineShaderStageCreateInfo; 2]> = fragment_modules
            .iter()
            .map(|&fragment| {
                [
                    vk::PipelineShaderStageCreateInfo::default()
                        .stage(vk::ShaderStageFlags::VERTEX)
                        .module(vertex_module)
                        .name(entry_point),
                    vk::PipelineShaderStageCreateInfo::default()
                        .stage(vk::ShaderStageFlags::FRAGMENT)
                        .module(fragment)
                        .name(entry_point),
                ]
            })
            .collect();
        let pipeline_infos: Vec<vk::GraphicsPipelineCreateInfo> = stages
            .iter()
            .map(|stages| {
                vk::GraphicsPipelineCreateInfo::default()
                    .stages(stages)
                    .vertex_input_state(&vertex_input)
                    .input_assembly_state(&input_assembly)
                    .viewport_state(&viewport_state)
                    .rasterization_state(&rasterization)
                    .multisample_state(&multisample)
                    .color_blend_state(&color_blend)
                    .dynamic_state(&dynamic_state)
                    .layout(layout)
                    .render_pass(render_pass)
                    .subpass(0)
            })
            .collect();
        let pipelines = device
            .create_graphics_pipelines(vk::PipelineCache::null(), &pipeline_infos, None)
            .map_err(|(_, result)| result)?;

        // Record and submit.
        let pool_info = vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::TRANSIENT)
            .queue_family_index(self.queue_family);
        let pool = device.create_command_pool(&pool_info, None)?;
        let command_buffer_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        let command_buffer = device.allocate_command_buffers(&command_buffer_info)?[0];

        let begin_info = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        device.begin_command_buffer(command_buffer, &begin_info)?;

        let clear_value = vk::ClearValue {
            color: vk::ClearColorValue {
                float32: [0.0, 0.0, 0.0, 1.0],
            },
        };
        let clear_values = [clear_value];
        let render_begin = vk::RenderPassBeginInfo::default()
            .render_pass(render_pass)
            .framebuffer(framebuffer)
            .render_area(vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent: vk::Extent2D { width, height },
            })
            .clear_values(&clear_values);
        device.cmd_begin_render_pass(command_buffer, &render_begin, vk::SubpassContents::INLINE);
        for (draw, &pipeline) in draws.iter().zip(&pipelines) {
            let viewport = vk::Viewport {
                x: draw.x as f32,
                y: draw.y as f32,
                width: draw.width as f32,
                height: draw.height as f32,
                min_depth: draw.depth,
                max_depth: draw.depth,
            };
            let scissor = vk::Rect2D {
                offset: vk::Offset2D {
                    x: draw.x,
                    y: draw.y,
                },
                extent: vk::Extent2D {
                    width: draw.width,
                    height: draw.height,
                },
            };
            device.cmd_bind_pipeline(command_buffer, vk::PipelineBindPoint::GRAPHICS, pipeline);
            device.cmd_set_viewport(command_buffer, 0, &[viewport]);
            device.cmd_set_scissor(command_buffer, 0, &[scissor]);
            device.cmd_draw(command_buffer, 3, 1, 0, 0);
        }
        device.cmd_end_render_pass(command_buffer);

        let copy_region = vk::BufferImageCopy::default()
            .image_subresource(vk::ImageSubresourceLayers {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                mip_level: 0,
                base_array_layer: 0,
                layer_count: 1,
            })
            .image_extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            });
        device.cmd_copy_image_to_buffer(
            command_buffer,
            image,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            readback,
            &[copy_region],
        );
        device.end_command_buffer(command_buffer)?;

        let command_buffers = [command_buffer];
        let submit_info = vk::SubmitInfo::default().command_buffers(&command_buffers);
        device.queue_submit(self.queue, &[submit_info], vk::Fence::null())?;
        device.queue_wait_idle(self.queue)?;

        let mapped =
            device.map_memory(readback_memory, 0, byte_size, vk::MemoryMapFlags::empty())?;
        let mut out = vec![0_u8; byte_size as usize];
        std::ptr::copy_nonoverlapping(mapped.cast::<u8>(), out.as_mut_ptr(), out.len());
        device.unmap_memory(readback_memory);

        // Tear everything down so the validation scenario sees no leaks.
        device.destroy_command_pool(pool, None);
        for pipeline in pipelines {
            device.destroy_pipeline(pipeline, None);
        }
        device.destroy_pipeline_layout(layout, None);
        for module in fragment_modules {
            device.destroy_shader_module(module, None);
        }
        device.destroy_shader_module(vertex_module, None);
        device.destroy_buffer(readback, None);
        device.free_memory(readback_memory, None);
        device.destroy_framebuffer(framebuffer, None);
        device.destroy_render_pass(render_pass, None);
        device.destroy_image_view(view, None);
        device.destroy_image(image, None);
        device.free_memory(image_memory, None);
        Ok(out)
    }
}

impl Drop for Ctx {
    fn drop(&mut self) {
        unsafe {
            if self.device.handle() != vk::Device::null() {
                self.device.destroy_device(None);
            }
            if let Some(debug) = &self.debug {
                debug
                    .utils
                    .destroy_debug_utils_messenger(debug.messenger, None);
            }
            self.instance.destroy_instance(None);
        }
    }
}

fn find_memory_type(
    properties: &vk::PhysicalDeviceMemoryProperties,
    type_bits: u32,
    required: vk::MemoryPropertyFlags,
) -> Option<u32> {
    (0..properties.memory_type_count).find(|&index| {
        type_bits & (1 << index) != 0
            && properties.memory_types[index as usize]
                .property_flags
                .contains(required)
    })
}

/// Writes an RGBA8 image to a PNG file.
pub fn write_png(
    path: &Path,
    width: u32,
    height: u32,
    rgba: &[u8],
) -> Result<(), png::EncodingError> {
    let file = std::fs::File::create(path)?;
    let writer = std::io::BufWriter::new(file);
    let mut encoder = png::Encoder::new(writer, width, height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(rgba)
}
