mod build_support;

fn main() {
    let manifest_dir = std::path::PathBuf::from(
        std::env::var_os("CARGO_MANIFEST_DIR").expect("Cargo manifest directory"),
    );
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").expect("target operating system");
    let target_triple = std::env::var("TARGET").expect("Cargo target triple");
    let native_build_directory = std::env::var_os("AXKLIB_BUILD_DIRECTORY")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| build_support::default_native_build_directory(&manifest_dir));
    let version_metadata_path = std::env::var_os("AXKLIB_VERSION_METADATA_FILE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| native_build_directory.join("version_metadata.json"));
    let package_basename_path = std::env::var_os("AXKLIB_PACKAGE_BASENAME_FILE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| native_build_directory.join("package_basename.txt"));
    let identity =
        build_support::read_build_identity(&version_metadata_path, &package_basename_path)
            .unwrap_or_else(|message| panic!("{message}"));
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
    println!(
        "cargo:rustc-env=AXKDECK_SEMANTIC_VERSION={}",
        identity.semantic_version
    );
    println!(
        "cargo:rustc-env=AXKDECK_PROJECT_VERSION={}",
        identity.project_version
    );
    println!(
        "cargo:rustc-env=AXKDECK_SOURCE_IDENTITY={}",
        identity.source_identity
    );
    println!(
        "cargo:rustc-env=AXKDECK_RELEASE_TAG={}",
        identity.release_tag
    );
    println!("cargo:rustc-env=AXKDECK_IS_RELEASE={}", identity.is_release);
    println!("cargo:rerun-if-changed={}", server_source.display());
    println!("cargo:rerun-if-changed={}", version_metadata_path.display());
    println!("cargo:rerun-if-changed={}", package_basename_path.display());
    println!("cargo:rerun-if-env-changed=AXKLIB_SERVER_BINARY");
    println!("cargo:rerun-if-env-changed=AXKLIB_BUILD_DIRECTORY");
    println!("cargo:rerun-if-env-changed=AXKLIB_VERSION_METADATA_FILE");
    println!("cargo:rerun-if-env-changed=AXKLIB_PACKAGE_BASENAME_FILE");
}
