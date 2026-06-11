//! Renders a small game-like scene three times and writes PNG screenshots:
//!
//!   * `scene_normal.png`    — without the mirror layer (original materials)
//!   * `scene_depth.png`     — with the layer in depth mode
//!   * `scene_highlight.png` — depth mode with the "player" material
//!     highlighted in green and the "chest" material in yellow
//!
//! Usage: `scene <output-directory>`. The example builds and registers the
//! freshly compiled layer itself, so no `VK_ADD_LAYER_PATH` is required.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use mirror::hash::fnv1a64;
use mirror::include_spv;
use mirror::spirv::Spirv;
use vku::{Ctx, Draw, InitError};

const WIDTH: u32 = 512;
const HEIGHT: u32 = 512;

fn sky() -> Spirv {
    include_spv!("sky.frag")
}
fn floor() -> Spirv {
    include_spv!("floor.frag")
}
fn wall() -> Spirv {
    include_spv!("wall.frag")
}
fn chest() -> Spirv {
    include_spv!("chest.frag")
}
fn player() -> Spirv {
    include_spv!("player.frag")
}

/// A crude corridor: walls receding into the distance, two "players" and a
/// "chest" placed at different depths.
fn scene_draws() -> Vec<Draw> {
    let draw = |spirv: Spirv, x: i32, y: i32, width: u32, height: u32, depth: f32| Draw {
        frag_spv: spirv.words(),
        x,
        y,
        width,
        height,
        depth,
    };
    vec![
        draw(sky(), 0, 0, 512, 512, 0.98),
        draw(floor(), 0, 320, 512, 192, 0.90),
        draw(wall(), 0, 64, 96, 320, 0.85),
        draw(wall(), 416, 64, 96, 320, 0.85),
        draw(wall(), 96, 112, 64, 240, 0.70),
        draw(wall(), 352, 112, 64, 240, 0.70),
        draw(chest(), 296, 296, 72, 56, 0.55),
        draw(player(), 200, 200, 48, 168, 0.45),
        draw(player(), 96, 240, 64, 200, 0.30),
    ]
}

/// Renders the scene and writes it to `path`. Returns `Ok(false)` when there is
/// no Vulkan device, so the caller can exit with the 77 "skip" code.
fn render_to_png(enable_layer: bool, path: &Path) -> Result<bool, String> {
    let ctx = match Ctx::init(enable_layer, false) {
        Ok(ctx) => ctx,
        Err(InitError::NoDevice) => return Ok(false),
        Err(error) => return Err(format!("context creation failed: {error}")),
    };
    let image = ctx
        .render(WIDTH, HEIGHT, &scene_draws())
        .map_err(|result| format!("render failed: {result:?}"))?;
    vku::write_png(path, WIDTH, HEIGHT, &image)
        .map_err(|error| format!("failed to write {}: {error}", path.display()))?;
    println!("wrote {}", path.display());
    Ok(true)
}

fn run() -> Result<bool, String> {
    let out_dir: PathBuf = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));

    std::env::remove_var("MIRROR_MODE");
    std::env::remove_var("MIRROR_HIGHLIGHT");

    if !render_to_png(false, &out_dir.join("scene_normal.png"))? {
        return Ok(false);
    }

    std::env::set_var("MIRROR_MODE", "depth");
    if !render_to_png(true, &out_dir.join("scene_depth.png"))? {
        return Ok(false);
    }

    let highlights = format!(
        "{:016x}=00ff00,{:016x}=ffcc00",
        fnv1a64(player().bytes()),
        fnv1a64(chest().bytes()),
    );
    println!("MIRROR_HIGHLIGHT={highlights}");
    std::env::set_var("MIRROR_HIGHLIGHT", highlights);
    render_to_png(true, &out_dir.join("scene_highlight.png"))
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => {
            eprintln!("no Vulkan device available");
            // 77 is the conventional "skipped" status used by the test suite.
            ExitCode::from(77)
        }
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
