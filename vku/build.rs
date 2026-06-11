//! Compiles the harness's fullscreen-triangle vertex shader to SPIR-V with
//! `glslangValidator`, writing `fullscreen.vert.spv` into `OUT_DIR` for
//! `include_bytes!` to embed.

use std::path::Path;
use std::process::Command;

fn main() {
    let source = "shaders/fullscreen.vert";
    let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR is set by cargo");
    let glslang = std::env::var("GLSLANG_VALIDATOR").unwrap_or_else(|_| "glslangValidator".into());

    println!("cargo:rerun-if-changed={source}");
    println!("cargo:rerun-if-env-changed=GLSLANG_VALIDATOR");

    let output = Path::new(&out_dir).join("fullscreen.vert.spv");
    let status = Command::new(&glslang)
        .args(["-V", source, "-o"])
        .arg(&output)
        .status()
        .unwrap_or_else(|err| panic!("failed to run {glslang}: {err}"));
    assert!(status.success(), "glslangValidator failed for {source}");
}
