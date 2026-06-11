//! End-to-end test of the mirror layer on a real Vulkan implementation (a CPU
//! driver such as lavapipe is enough, so this runs headless in CI).
//!
//! The scene is two half-screen rectangles:
//!   * left  — a "red material"  at depth 0.25
//!   * right — a "blue material" at depth 0.75
//!
//! Each scenario fully controls the layer configuration through environment
//! variables, so they run sequentially in a single test to avoid races on the
//! process environment (the layer reads it when the device is created).

use mirror::hash::fnv1a64;
use mirror::include_spv;
use mirror::spirv::Spirv;
use vku::{Ctx, Draw, InitError};

const WIDTH: u32 = 256;
const HEIGHT: u32 = 256;

fn red() -> Spirv {
    include_spv!("red.frag")
}
fn blue() -> Spirv {
    include_spv!("blue.frag")
}

#[derive(Clone, Copy)]
struct Pixel {
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

fn pixel_at(image: &[u8], x: u32, y: u32) -> Pixel {
    let offset = ((y * WIDTH + x) * 4) as usize;
    Pixel {
        r: image[offset],
        g: image[offset + 1],
        b: image[offset + 2],
        a: image[offset + 3],
    }
}

#[track_caller]
fn expect_pixel(what: &str, pixel: Pixel, r: u8, g: u8, b: u8) {
    let tolerance = 3_i32;
    let near =
        |actual: u8, expected: u8| (i32::from(actual) - i32::from(expected)).abs() <= tolerance;
    assert!(
        near(pixel.r, r) && near(pixel.g, g) && near(pixel.b, b) && pixel.a == 255,
        "{what}: expected ~({r}, {g}, {b}, 255), got ({}, {}, {}, {})",
        pixel.r,
        pixel.g,
        pixel.b,
        pixel.a,
    );
}

fn clear_env() {
    std::env::remove_var("MIRROR_MODE");
    std::env::remove_var("MIRROR_HIGHLIGHT");
    std::env::remove_var("MIRROR_LOG");
}

/// Renders the two-rectangle scene, returning the left and right sample pixels
/// and the number of validation errors, or `None` if there is no Vulkan device.
fn render_scene(enable_mirror: bool, enable_validation: bool) -> Option<(Pixel, Pixel, u64)> {
    let ctx = match Ctx::init(enable_mirror, enable_validation) {
        Ok(ctx) => ctx,
        Err(InitError::NoDevice) => return None,
        Err(error) => panic!("context creation failed: {error}"),
    };

    let draws = [
        Draw {
            frag_spv: red().words(),
            x: 0,
            y: 0,
            width: WIDTH / 2,
            height: HEIGHT,
            depth: 0.25,
        },
        Draw {
            frag_spv: blue().words(),
            x: (WIDTH / 2) as i32,
            y: 0,
            width: WIDTH / 2,
            height: HEIGHT,
            depth: 0.75,
        },
    ];
    let image = ctx.render(WIDTH, HEIGHT, &draws).expect("render failed");
    let left = pixel_at(&image, WIDTH / 4, HEIGHT / 2);
    let right = pixel_at(&image, 3 * WIDTH / 4, HEIGHT / 2);
    Some((left, right, ctx.validation_errors()))
}

#[test]
fn render_scenarios() {
    // The layer must be locatable and visible to the loader (`enumerate`).
    assert!(
        vku::install_layer(),
        "could not locate the built mirror layer library"
    );
    assert!(
        vku::layer_available(vku::MIRROR_LAYER_NAME),
        "layer is not visible to vkEnumerateInstanceLayerProperties",
    );

    // nolayer: baseline without the layer — original material colors.
    clear_env();
    let Some((left, right, _)) = render_scene(false, false) else {
        eprintln!("no Vulkan device available, skipping render scenarios");
        return;
    };
    expect_pixel("nolayer left (red)", left, 255, 0, 0);
    expect_pixel("nolayer right (blue)", right, 0, 0, 255);

    // off: layer enabled but MIRROR_MODE=off — passthrough.
    clear_env();
    std::env::set_var("MIRROR_MODE", "off");
    let (left, right, _) = render_scene(true, false).expect("device disappeared mid-run");
    expect_pixel("off left (red)", left, 255, 0, 0);
    expect_pixel("off right (blue)", right, 0, 0, 255);

    // depth: grayscale equal to each draw's depth (0.25 -> 64, 0.75 -> 191).
    clear_env();
    std::env::set_var("MIRROR_MODE", "depth");
    let (left, right, _) = render_scene(true, false).expect("device disappeared mid-run");
    expect_pixel("depth left (0.25)", left, 64, 64, 64);
    expect_pixel("depth right (0.75)", right, 191, 191, 191);

    // highlight: the blue material is painted green via its hash. The color is
    // scaled by 1 - 0.5 * depth = 0.625 -> 159/255.
    clear_env();
    std::env::set_var("MIRROR_MODE", "depth");
    let blue_hash = fnv1a64(blue().bytes());
    std::env::set_var("MIRROR_HIGHLIGHT", format!("{blue_hash:016x}=00ff00"));
    let (left, right, _) = render_scene(true, false).expect("device disappeared mid-run");
    expect_pixel("highlight left (0.25)", left, 64, 64, 64);
    expect_pixel("highlight right (green)", right, 0, 159, 0);

    // validation: depth mode under the Khronos validation layer, zero errors.
    if vku::layer_available(vku::VALIDATION_LAYER_NAME) {
        clear_env();
        std::env::set_var("MIRROR_MODE", "depth");
        let (left, right, errors) = render_scene(true, true).expect("device disappeared mid-run");
        expect_pixel("validation left (0.25)", left, 64, 64, 64);
        expect_pixel("validation right (0.75)", right, 191, 191, 191);
        assert_eq!(errors, 0, "validation layer reported {errors} error(s)");
    } else {
        eprintln!("VK_LAYER_KHRONOS_validation not installed, skipping validation scenario");
    }

    clear_env();
}
