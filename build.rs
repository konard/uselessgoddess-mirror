//! Compiles every GLSL shader the crate embeds into SPIR-V with
//! `glslangValidator`, writing `<name>.spv` into `OUT_DIR`. The layer, the
//! integration tests and the `scene` example then pull those binaries in with
//! `include_spv!` (see `src/spirv.rs`), so no SPIR-V blobs are checked in.

use std::path::Path;
use std::process::Command;

fn main() {
    // (source path, output stem) for every shader this package needs.
    let shaders = [
        ("shaders/mirror.frag", "mirror.frag"),
        ("tests/shaders/red.frag", "red.frag"),
        ("tests/shaders/blue.frag", "blue.frag"),
        ("examples/shaders/sky.frag", "sky.frag"),
        ("examples/shaders/wall.frag", "wall.frag"),
        ("examples/shaders/floor.frag", "floor.frag"),
        ("examples/shaders/player.frag", "player.frag"),
        ("examples/shaders/chest.frag", "chest.frag"),
    ];

    let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR is set by cargo");
    let glslang = std::env::var("GLSLANG_VALIDATOR").unwrap_or_else(|_| "glslangValidator".into());

    for (source, stem) in shaders {
        println!("cargo:rerun-if-changed={source}");
        let output = Path::new(&out_dir).join(format!("{stem}.spv"));
        let status = Command::new(&glslang)
            .args(["-V", source, "-o"])
            .arg(&output)
            .status()
            .unwrap_or_else(|err| panic!("failed to run {glslang}: {err}"));
        assert!(status.success(), "glslangValidator failed for {source}");
    }

    println!("cargo:rerun-if-env-changed=GLSLANG_VALIDATOR");
}
