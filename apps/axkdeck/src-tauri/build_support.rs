use std::ffi::OsStr;
use std::path::{Path, PathBuf};

fn executable_name(target_os: &str) -> Result<&'static str, String> {
    match target_os {
        "linux" | "macos" => Ok("axklib-server"),
        "windows" => Ok("axklib-server.exe"),
        other => Err(format!(
            "unsupported desktop target operating system: {other}"
        )),
    }
}

pub fn default_server_binary(manifest_dir: &Path, target_os: &str) -> Result<PathBuf, String> {
    Ok(manifest_dir
        .join("../../..")
        .join("build/native/release/apps/server")
        .join(executable_name(target_os)?))
}

pub fn resolve_server_binary(
    manifest_dir: &Path,
    target_os: &str,
    configured: Option<&OsStr>,
) -> Result<PathBuf, String> {
    let candidate = configured
        .map(PathBuf::from)
        .map(Ok)
        .unwrap_or_else(|| default_server_binary(manifest_dir, target_os))?;
    candidate.canonicalize().map_err(|error| {
        format!(
            "axklib-server is missing at {}: {error}. Build it with `cmake --build --preset release --target axklib_server` or set AXKLIB_SERVER_BINARY",
            candidate.display()
        )
    })
}

pub fn sidecar_path(
    manifest_dir: &Path,
    target_os: &str,
    target_triple: &str,
) -> Result<PathBuf, String> {
    let extension = if target_os == "windows" { ".exe" } else { "" };
    executable_name(target_os)?;
    Ok(manifest_dir
        .join("binaries")
        .join(format!("axklib-server-{target_triple}{extension}")))
}
