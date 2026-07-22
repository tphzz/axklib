#[path = "../build_support.rs"]
mod build_support;

use std::path::Path;

#[test]
fn default_server_uses_the_monorepo_release_build() {
    let manifest = Path::new("/source/apps/axkdeck/src-tauri");
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
