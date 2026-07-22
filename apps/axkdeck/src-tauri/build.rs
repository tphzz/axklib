mod build_support;

fn main() {
    let manifest_dir = std::path::PathBuf::from(
        std::env::var_os("CARGO_MANIFEST_DIR").expect("Cargo manifest directory"),
    );
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").expect("target operating system");
    let target_triple = std::env::var("TARGET").expect("Cargo target triple");
    let configured_server = std::env::var_os("AXKLIB_SERVER_BINARY");
    let server_source = build_support::resolve_server_binary(
        &manifest_dir,
        &target_os,
        configured_server.as_deref(),
    )
    .unwrap_or_else(|message| panic!("{message}"));
    let sidecar_path = build_support::sidecar_path(&manifest_dir, &target_os, &target_triple)
        .unwrap_or_else(|message| panic!("{message}"));

    std::fs::create_dir_all(
        sidecar_path
            .parent()
            .expect("Tauri sidecar destination directory"),
    )
    .expect("create Tauri sidecar directory");
    std::fs::copy(&server_source, &sidecar_path).unwrap_or_else(|error| {
        panic!(
            "stage axklib-server from {} to {}: {error}",
            server_source.display(),
            sidecar_path.display()
        )
    });

    tauri_build::build();
    println!("cargo:rerun-if-changed={}", server_source.display());
    println!("cargo:rerun-if-env-changed=AXKLIB_SERVER_BINARY");
}
