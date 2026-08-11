use std::ffi::OsStr;
use std::path::{Path, PathBuf};

use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct VersionMetadata {
    schema_version: u32,
    semantic_version: String,
    project_version: String,
    major: u64,
    minor: u64,
    patch: u64,
    release_tag: String,
    is_release: bool,
    is_prerelease: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BuildIdentity {
    pub semantic_version: String,
    pub project_version: String,
    pub source_identity: String,
    pub release_tag: String,
    pub is_release: bool,
}

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

pub fn default_native_build_directory(manifest_dir: &Path) -> PathBuf {
    manifest_dir.join("../../..").join("build/native/release")
}

pub fn read_build_identity(
    version_metadata_path: &Path,
    package_basename_path: &Path,
) -> Result<BuildIdentity, String> {
    let metadata: VersionMetadata =
        serde_json::from_slice(&std::fs::read(version_metadata_path).map_err(|error| {
            format!(
                "read native version metadata at {}: {error}",
                version_metadata_path.display()
            )
        })?)
        .map_err(|error| format!("parse native version metadata: {error}"))?;
    if metadata.schema_version != 1 {
        return Err(format!(
            "unsupported native version metadata schema {}",
            metadata.schema_version
        ));
    }
    if metadata.semantic_version.is_empty() || metadata.project_version.is_empty() {
        return Err("native version metadata is incomplete".to_owned());
    }
    if metadata.project_version
        != format!("{}.{}.{}", metadata.major, metadata.minor, metadata.patch)
    {
        return Err("native project version does not match its numeric components".to_owned());
    }
    if metadata.is_release {
        if metadata.release_tag != format!("v{}", metadata.semantic_version) {
            return Err("native release tag does not match its semantic version".to_owned());
        }
    } else if metadata.semantic_version != "0.0.0"
        || !metadata.release_tag.is_empty()
        || metadata.is_prerelease
    {
        return Err("native development version metadata is inconsistent".to_owned());
    }

    let package_text = std::fs::read_to_string(package_basename_path).map_err(|error| {
        format!(
            "read native package basename at {}: {error}",
            package_basename_path.display()
        )
    })?;
    let Some(package_basename) = package_text
        .strip_suffix("\r\n")
        .or_else(|| package_text.strip_suffix('\n'))
    else {
        return Err("native package basename is not newline-terminated".to_owned());
    };
    if package_basename.contains(['\r', '\n']) {
        return Err("native package basename contains multiple lines".to_owned());
    }
    let Some(source_identity) = package_basename.strip_prefix("axklib-") else {
        return Err("native package basename has the wrong product prefix".to_owned());
    };
    if source_identity.is_empty()
        || !source_identity
            .bytes()
            .all(|value| value.is_ascii_alphanumeric() || b"._-".contains(&value))
    {
        return Err("native package basename contains an invalid source identity".to_owned());
    }

    Ok(BuildIdentity {
        semantic_version: metadata.semantic_version,
        project_version: metadata.project_version,
        source_identity: source_identity.to_owned(),
        release_tag: metadata.release_tag,
        is_release: metadata.is_release,
    })
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
