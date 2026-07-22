#[path = "../build_support.rs"]
mod build_support;

use std::path::{Path, PathBuf};

fn temporary_directory(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "axkdeck-build-support-{name}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("system clock")
            .as_nanos()
    ))
}

#[test]
fn default_server_uses_the_monorepo_release_build() {
    let manifest = Path::new("/source/apps/axkdeck/src-tauri");
    assert_eq!(
        build_support::default_native_build_directory(manifest),
        Path::new("/source/apps/axkdeck/src-tauri/../../../build/native/release")
    );
    assert_eq!(
        build_support::default_server_binary(manifest, "linux").expect("Linux path"),
        Path::new(
            "/source/apps/axkdeck/src-tauri/../../../build/native/release/apps/server/axklib-server"
        )
    );
    assert_eq!(
        build_support::default_server_binary(manifest, "windows").expect("Windows path"),
        Path::new(
            "/source/apps/axkdeck/src-tauri/../../../build/native/release/apps/server/axklib-server.exe"
        )
    );
}

#[test]
fn explicit_server_override_is_used_without_a_second_native_build() {
    let existing = std::env::current_exe().expect("current test executable");
    assert_eq!(
        build_support::resolve_server_binary(
            Path::new("/unused"),
            "linux",
            Some(existing.as_os_str()),
        )
        .expect("override"),
        existing.canonicalize().expect("canonical test executable")
    );
}

#[test]
fn sidecar_name_carries_the_tauri_target_triple() {
    let manifest = Path::new("/source/apps/axkdeck/src-tauri");
    assert_eq!(
        build_support::sidecar_path(manifest, "windows", "aarch64-pc-windows-msvc")
            .expect("Windows sidecar"),
        manifest.join("binaries/axklib-server-aarch64-pc-windows-msvc.exe")
    );
    assert!(build_support::sidecar_path(manifest, "android", "aarch64-linux-android").is_err());
}

#[test]
fn build_identity_comes_from_the_native_generated_contract() {
    let directory = temporary_directory("identity");
    std::fs::create_dir_all(&directory).expect("create test directory");
    let metadata = directory.join("version_metadata.json");
    let package = directory.join("package_basename.txt");
    std::fs::write(
        &metadata,
        r#"{"schema_version":1,"semantic_version":"1.2.3-rc.1","project_version":"1.2.3","major":1,"minor":2,"patch":3,"release_tag":"v1.2.3-rc.1","is_release":true,"is_prerelease":true}"#,
    )
    .expect("write version metadata");
    std::fs::write(&package, "axklib-v1.2.3-rc.1-a1b2c3d\n").expect("write package basename");

    let identity =
        build_support::read_build_identity(&metadata, &package).expect("read build identity");
    assert_eq!(identity.semantic_version, "1.2.3-rc.1");
    assert_eq!(identity.project_version, "1.2.3");
    assert_eq!(identity.source_identity, "v1.2.3-rc.1-a1b2c3d");
    assert_eq!(identity.release_tag, "v1.2.3-rc.1");
    assert!(identity.is_release);

    std::fs::remove_dir_all(directory).expect("remove test directory");
}

#[test]
fn inconsistent_development_identity_is_rejected() {
    let directory = temporary_directory("invalid");
    std::fs::create_dir_all(&directory).expect("create test directory");
    let metadata = directory.join("version_metadata.json");
    let package = directory.join("package_basename.txt");
    std::fs::write(
        &metadata,
        r#"{"schema_version":1,"semantic_version":"1.2.3","project_version":"1.2.3","major":1,"minor":2,"patch":3,"release_tag":"","is_release":false,"is_prerelease":false}"#,
    )
    .expect("write version metadata");
    std::fs::write(&package, "axklib-main-a1b2c3d\n").expect("write package basename");

    let error = build_support::read_build_identity(&metadata, &package)
        .expect_err("inconsistent development identity must fail");
    assert!(error.contains("development version"), "{error}");

    std::fs::remove_dir_all(directory).expect("remove test directory");
}
