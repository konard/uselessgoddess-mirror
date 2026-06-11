//! Locating and registering the freshly built `mirror` layer so the Vulkan
//! loader can find it without a system-wide install.
//!
//! The `cdylib` (`libmirror.so`) lands next to the test/example binaries under
//! `target/<profile>/`. We synthesize a layer manifest pointing at it and add
//! its directory to `VK_ADD_LAYER_PATH`, exactly once per process.

use std::ffi::CStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// The layer name the loader advertises it under.
pub const MIRROR_LAYER_NAME: &CStr = c"VK_LAYER_MIRROR_mirror";
/// The Khronos validation layer, enabled by the validation test scenario.
pub const VALIDATION_LAYER_NAME: &CStr = c"VK_LAYER_KHRONOS_validation";

#[cfg(target_os = "windows")]
const LIBRARY_FILE: &str = "mirror.dll";
#[cfg(target_os = "macos")]
const LIBRARY_FILE: &str = "libmirror.dylib";
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
const LIBRARY_FILE: &str = "libmirror.so";

/// Builds a manifest for the locally built layer and points
/// `VK_ADD_LAYER_PATH` at it. Idempotent: the work happens once per process.
///
/// Returns `false` if the layer shared object could not be located, in which
/// case callers should treat the mirror layer as unavailable.
pub fn install() -> bool {
    static INSTALLED: OnceLock<bool> = OnceLock::new();
    *INSTALLED.get_or_init(install_once)
}

fn install_once() -> bool {
    let Some(library) = find_layer_library() else {
        return false;
    };
    let manifest_dir = library.with_file_name("mirror-layer.d");
    if fs::create_dir_all(&manifest_dir).is_err() {
        return false;
    }
    let manifest = manifest_dir.join("VkLayer_MIRROR_mirror.json");
    if fs::write(&manifest, manifest_json(&library)).is_err() {
        return false;
    }
    prepend_layer_path(&manifest_dir);
    true
}

fn manifest_json(library: &Path) -> String {
    // JSON-escape the path's backslashes for Windows; forward slashes are fine.
    let library = library.display().to_string().replace('\\', "\\\\");
    format!(
        r#"{{
  "file_format_version": "1.2.0",
  "layer": {{
    "name": "VK_LAYER_MIRROR_mirror",
    "type": "GLOBAL",
    "library_path": "{library}",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "Renders depth instead of materials, with configurable highlights"
  }}
}}
"#
    )
}

/// Walks up from the current executable looking for the layer shared object,
/// which Cargo places in the target profile directory (a couple of levels
/// above `deps/`).
fn find_layer_library() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent();
    for _ in 0..4 {
        let Some(current) = dir else { break };
        let candidate = current.join(LIBRARY_FILE);
        if candidate.is_file() {
            return Some(candidate);
        }
        dir = current.parent();
    }
    None
}

fn prepend_layer_path(dir: &Path) {
    let mut paths = vec![dir.to_path_buf()];
    if let Some(existing) = std::env::var_os("VK_ADD_LAYER_PATH") {
        paths.extend(std::env::split_paths(&existing));
    }
    if let Ok(joined) = std::env::join_paths(paths) {
        std::env::set_var("VK_ADD_LAYER_PATH", joined);
    }
}
