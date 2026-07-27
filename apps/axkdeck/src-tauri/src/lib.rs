mod desktop_preferences;
mod file_publication;
mod remote_settings;
mod server_sidecar;

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;
use tauri::{AppHandle, Manager, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_fs::FsExt;
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};

use desktop_preferences::DesktopPreferencesStore;

const LOG_FILE_SIZE: u128 = 5 * 1024 * 1024;
const RETAINED_LOG_FILES: usize = 3;
const MAX_RETAINED_PACKAGE_BYTES: u64 = 4 * 1024 * 1024 * 1024;
const MAX_RETAINED_SFZ_EXPORT_BYTES: u64 = 4 * 1024 * 1024 * 1024;
const MAX_RETAINED_SFZ_EXPORT_ENTRIES: usize = 100_000;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct DesktopBuildInfo {
    schema_version: u32,
    semantic_version: &'static str,
    project_version: &'static str,
    source_identity: &'static str,
    release_tag: &'static str,
    is_release: bool,
}

fn current_build_info() -> DesktopBuildInfo {
    DesktopBuildInfo {
        schema_version: 1,
        semantic_version: env!("AXKDECK_SEMANTIC_VERSION"),
        project_version: env!("AXKDECK_PROJECT_VERSION"),
        source_identity: env!("AXKDECK_SOURCE_IDENTITY"),
        release_tag: env!("AXKDECK_RELEASE_TAG"),
        is_release: env!("AXKDECK_IS_RELEASE") == "true",
    }
}

fn parse_log_level(value: Option<&str>) -> log::LevelFilter {
    match value.map(str::trim).map(str::to_ascii_lowercase).as_deref() {
        Some("trace") => log::LevelFilter::Trace,
        Some("debug") => log::LevelFilter::Debug,
        Some("warn") => log::LevelFilter::Warn,
        Some("error") => log::LevelFilter::Error,
        Some("off") => log::LevelFilter::Off,
        Some("info") | None => log::LevelFilter::Info,
        Some(_) => log::LevelFilter::Info,
    }
}

fn configured_log_level() -> log::LevelFilter {
    let value = std::env::var("AXKDECK_LOG_LEVEL").ok();
    let level = parse_log_level(value.as_deref());
    if value.as_deref().is_some_and(|value| {
        !matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "trace" | "debug" | "info" | "warn" | "error" | "off"
        )
    }) {
        eprintln!("invalid AXKDECK_LOG_LEVEL; using info");
    }
    level
}

fn log_level_name(level: log::LevelFilter) -> &'static str {
    match level {
        log::LevelFilter::Off => "off",
        log::LevelFilter::Error => "error",
        log::LevelFilter::Warn => "warn",
        log::LevelFilter::Info => "info",
        log::LevelFilter::Debug => "debug",
        log::LevelFilter::Trace => "trace",
    }
}

#[cfg(test)]
mod tests {
    use std::path::{Path, PathBuf};

    use super::{
        checked_tar_path, current_build_info, extract_sfz_tar, normalize_package_destination,
        normalize_sfz_destination, parse_log_level, valid_retained_content_path,
    };

    #[test]
    fn log_level_parser_accepts_supported_values_and_defaults_to_info() {
        assert_eq!(parse_log_level(None), log::LevelFilter::Info);
        assert_eq!(parse_log_level(Some(" DEBUG ")), log::LevelFilter::Debug);
        assert_eq!(parse_log_level(Some("trace")), log::LevelFilter::Trace);
        assert_eq!(parse_log_level(Some("warn")), log::LevelFilter::Warn);
        assert_eq!(parse_log_level(Some("error")), log::LevelFilter::Error);
        assert_eq!(parse_log_level(Some("off")), log::LevelFilter::Off);
        assert_eq!(parse_log_level(Some("verbose")), log::LevelFilter::Info);
    }

    #[test]
    fn build_info_exposes_the_native_build_identity() {
        let build = current_build_info();
        assert_eq!(build.schema_version, 1);
        assert!(!build.semantic_version.is_empty());
        assert!(!build.project_version.is_empty());
        assert!(!build.source_identity.is_empty());
        assert_eq!(build.is_release, !build.release_tag.is_empty());
    }

    #[test]
    fn package_destination_requires_the_requested_package_extension() {
        assert_eq!(
            normalize_package_destination(PathBuf::from("Volume"), "axkvol").unwrap(),
            Path::new("Volume.axkvol")
        );
        assert_eq!(
            normalize_package_destination(PathBuf::from("Sample"), "axksbnk").unwrap(),
            Path::new("Sample.axksbnk")
        );
        assert_eq!(
            normalize_package_destination(PathBuf::from("Selection.axkpkg"), "axkpkg").unwrap(),
            Path::new("Selection.axkpkg")
        );
        assert!(normalize_package_destination(PathBuf::from("Volume.axkvol"), "axkpkg").is_err());
    }

    #[test]
    fn retained_package_path_accepts_only_one_archive_identifier() {
        assert!(valid_retained_content_path(
            "/api/v1/download-archives/0123456789abcdef/content"
        ));
        assert!(!valid_retained_content_path(
            "/api/v1/download-archives/../system/version/content"
        ));
        assert!(!valid_retained_content_path(
            "/api/v1/download-archives/id/extra/content"
        ));
        assert!(!valid_retained_content_path(
            "/api/v1/download-archives/id/content?range=all"
        ));
    }

    fn tar_header(path: &str, size: usize) -> [u8; 512] {
        let mut header = [0_u8; 512];
        header[..path.len()].copy_from_slice(path.as_bytes());
        header[100..108].copy_from_slice(b"0000755\0");
        header[108..116].copy_from_slice(b"0000000\0");
        header[116..124].copy_from_slice(b"0000000\0");
        let size_field = format!("{size:011o}\0");
        header[124..136].copy_from_slice(size_field.as_bytes());
        header[136..148].copy_from_slice(b"00000000000\0");
        header[148..156].fill(b' ');
        header[156] = b'0';
        header[257..263].copy_from_slice(b"ustar\0");
        header[263..265].copy_from_slice(b"00");
        let checksum = header.iter().map(|byte| u64::from(*byte)).sum::<u64>();
        let checksum_field = format!("{checksum:06o}\0 ");
        header[148..156].copy_from_slice(checksum_field.as_bytes());
        header
    }

    #[test]
    fn sfz_destination_must_be_new_but_uses_an_existing_parent() {
        let root =
            std::env::temp_dir().join(format!("axkdeck-sfz-destination-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir(&root).expect("create test root");
        assert_eq!(
            normalize_sfz_destination(root.join("Instrument")).expect("normalize destination"),
            root.canonicalize()
                .expect("canonical root")
                .join("Instrument")
        );
        std::fs::create_dir(root.join("Existing")).expect("create existing destination");
        assert!(normalize_sfz_destination(root.join("Existing")).is_err());
        std::fs::remove_dir_all(root).expect("remove test root");
    }

    #[test]
    fn sfz_tar_extraction_rejects_traversal_and_publishes_a_new_folder() {
        let root =
            std::env::temp_dir().join(format!("axkdeck-sfz-extraction-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir(&root).expect("create test root");
        let archive_path = root.join("export.tar");
        let mut archive = std::fs::OpenOptions::new()
            .create_new(true)
            .read(true)
            .write(true)
            .open(&archive_path)
            .expect("create test archive");
        let header = tar_header("Instrument.sfz", 9);
        std::io::Write::write_all(&mut archive, &header).expect("write header");
        std::io::Write::write_all(&mut archive, b"<region>\n").expect("write payload");
        std::io::Write::write_all(&mut archive, &[0_u8; 503]).expect("write padding");
        std::io::Write::write_all(&mut archive, &[0_u8; 1024]).expect("write end blocks");
        extract_sfz_tar(&mut archive, &root.join("Instrument")).expect("extract SFZ archive");
        assert_eq!(
            std::fs::read_to_string(root.join("Instrument/Instrument.sfz"))
                .expect("read extracted SFZ"),
            "<region>\n"
        );

        let unsafe_header = tar_header("../escape.sfz", 0);
        assert!(checked_tar_path(&unsafe_header).is_err());
        assert!(!root.join("escape.sfz").exists());
        std::fs::remove_dir_all(root).expect("remove test root");
    }
}

struct WorkspaceCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

struct PackageSaveCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

struct SfzSaveCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct WorkspaceCandidate {
    candidate_id: String,
    suggested_name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PackageSaveCandidate {
    candidate_id: String,
    filename: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SfzSaveCandidate {
    candidate_id: String,
    directory_name: String,
}

fn candidate_id() -> Result<String, String> {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "system clock is unavailable".to_owned())?
        .as_nanos();
    Ok(format!("{}-{nonce:x}", std::process::id()))
}

fn normalize_package_destination(
    mut path: PathBuf,
    expected_extension: &str,
) -> Result<PathBuf, String> {
    match path.extension().and_then(|value| value.to_str()) {
        None => {
            path.set_extension(expected_extension);
        }
        Some(extension) if extension.eq_ignore_ascii_case(expected_extension) => {}
        Some(_) => {
            return Err(format!(
                "the selected destination must end in .{expected_extension}"
            ));
        }
    }
    Ok(path)
}

fn normalize_sfz_destination(path: PathBuf) -> Result<PathBuf, String> {
    let parent = path
        .parent()
        .filter(|value| !value.as_os_str().is_empty())
        .ok_or_else(|| "the selected SFZ destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve SFZ destination directory: {error}"))?;
    if !parent.is_dir() {
        return Err("the selected SFZ destination parent is unavailable".to_owned());
    }
    let name = path
        .file_name()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| "the selected SFZ destination has no folder name".to_owned())?;
    if name == "." || name == ".." {
        return Err("the selected SFZ destination folder name is invalid".to_owned());
    }
    let destination = parent.join(name);
    if destination.exists() {
        return Err("the selected SFZ export folder already exists".to_owned());
    }
    Ok(destination)
}

fn valid_retained_content_path(path: &str) -> bool {
    path.strip_prefix("/api/v1/download-archives/")
        .and_then(|value| value.strip_suffix("/content"))
        .is_some_and(|archive_id| {
            !archive_id.is_empty() && archive_id.bytes().all(|byte| byte.is_ascii_alphanumeric())
        })
}

#[tauri::command]
async fn select_local_package(
    app: AppHandle,
    window: WebviewWindow,
) -> Result<Option<String>, String> {
    let scope_window = window.clone();
    let selected = tauri::async_runtime::spawn_blocking(move || {
        app.dialog()
            .file()
            .set_title("Choose axklib package")
            .add_filter(
                "axklib packages",
                &[
                    "axkvol", "axkprg", "axksbac", "axksbnk", "axksmpl", "axkpkg",
                ],
            )
            .set_parent(&window)
            .blocking_pick_file()
    })
    .await
    .map_err(|error| format!("open package file picker: {error}"))?;
    selected
        .map(|file| {
            let path = file
                .into_path()
                .map_err(|_| "the selected package is not a local filesystem path".to_owned())?
                .canonicalize()
                .map_err(|error| format!("resolve selected package: {error}"))?;
            if !path.is_file() {
                return Err("the selected package is not a regular file".to_owned());
            }
            scope_window
                .fs_scope()
                .allow_file(&path)
                .map_err(|error| format!("allow selected package for reading: {error}"))?;
            scope_window
                .state::<tauri::scope::Scopes>()
                .allow_file(&path)
                .map_err(|error| format!("allow selected package for the webview: {error}"))?;
            Ok(path.to_string_lossy().into_owned())
        })
        .transpose()
}

#[tauri::command]
async fn select_local_package_destination(
    app: AppHandle,
    window: WebviewWindow,
    suggested_name: String,
    state: State<'_, Mutex<PackageSaveCandidateStore>>,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<PackageSaveCandidate>, String> {
    let expected_extension = Path::new(&suggested_name)
        .extension()
        .and_then(|value| value.to_str())
        .filter(|value| {
            matches!(
                *value,
                "axkvol" | "axkprg" | "axksbac" | "axksbnk" | "axksmpl" | "axkpkg"
            )
        })
        .ok_or_else(|| "the suggested package filename has an unsupported extension".to_owned())?
        .to_owned();
    let picker_extension = expected_extension.clone();
    let starting_directory = match preferences.lock() {
        Ok(preferences) => preferences.package_export_directory(),
        Err(_) => {
            log::warn!("desktop preference state is unavailable; using the platform save location");
            None
        }
    };
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Save axklib package")
            .add_filter("axklib package", &[picker_extension.as_str()])
            .set_file_name(suggested_name)
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_save_file()
    })
    .await
    .map_err(|error| format!("open package save picker: {error}"))?;
    let Some(selected) = selected else {
        return Ok(None);
    };
    let path = selected
        .into_path()
        .map_err(|_| "the selected destination is not a local filesystem path".to_owned())?;
    let path = normalize_package_destination(path, &expected_extension)?;
    let filename = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "the selected destination has no valid filename".to_owned())?
        .to_owned();
    if let Some(directory) = path.parent() {
        match preferences.lock() {
            Ok(mut preferences) => {
                if let Err(error) = preferences.remember_package_export_directory(directory) {
                    log::warn!("could not persist the package export directory: {error}");
                }
            }
            Err(_) => {
                log::warn!(
                    "desktop preference state is unavailable; the package export directory was not retained"
                );
            }
        }
    }
    let candidate_id = candidate_id()?;
    let mut candidates = state
        .lock()
        .map_err(|_| "package destination state is unavailable".to_owned())?;
    candidates
        .values
        .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(300));
    candidates
        .values
        .insert(candidate_id.clone(), (path, Instant::now()));
    Ok(Some(PackageSaveCandidate {
        candidate_id,
        filename,
    }))
}

#[tauri::command]
async fn select_local_sfz_destination(
    app: AppHandle,
    window: WebviewWindow,
    suggested_name: String,
    state: State<'_, Mutex<SfzSaveCandidateStore>>,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<SfzSaveCandidate>, String> {
    if suggested_name.is_empty()
        || suggested_name == "."
        || suggested_name == ".."
        || suggested_name.contains('/')
        || suggested_name.contains('\\')
    {
        return Err("the suggested SFZ export folder name is invalid".to_owned());
    }
    let starting_directory = match preferences.lock() {
        Ok(preferences) => preferences.sfz_export_directory(),
        Err(_) => {
            log::warn!("desktop preference state is unavailable; using the platform save location");
            None
        }
    };
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Save SFZ export folder")
            .set_file_name(suggested_name)
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_save_file()
    })
    .await
    .map_err(|error| format!("open SFZ export picker: {error}"))?;
    let Some(selected) = selected else {
        return Ok(None);
    };
    let destination = normalize_sfz_destination(
        selected
            .into_path()
            .map_err(|_| "the selected destination is not a local filesystem path".to_owned())?,
    )?;
    let directory_name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "the selected SFZ destination has no valid folder name".to_owned())?
        .to_owned();
    if let Some(directory) = destination.parent() {
        match preferences.lock() {
            Ok(mut preferences) => {
                if let Err(error) = preferences.remember_sfz_export_directory(directory) {
                    log::warn!("could not persist the SFZ export directory: {error}");
                }
            }
            Err(_) => {
                log::warn!(
                    "desktop preference state is unavailable; the SFZ export directory was not retained"
                );
            }
        }
    }
    let candidate_id = candidate_id()?;
    let mut candidates = state
        .lock()
        .map_err(|_| "SFZ destination state is unavailable".to_owned())?;
    candidates
        .values
        .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(300));
    candidates
        .values
        .insert(candidate_id.clone(), (destination, Instant::now()));
    Ok(Some(SfzSaveCandidate {
        candidate_id,
        directory_name,
    }))
}

fn publish_downloaded_file(temporary: &Path, destination: &Path) -> Result<(), String> {
    let outcome = file_publication::publish_file(temporary, destination)?;
    if let Some(warning) = outcome.warning {
        log::warn!("{warning}");
    }
    Ok(())
}

fn download_retained_package(
    connection: server_sidecar::FrontendConnection,
    destination: PathBuf,
    content_path: String,
    expected_size: u64,
) -> Result<(), String> {
    if !valid_retained_content_path(&content_path) {
        return Err("the retained package path is invalid".to_owned());
    }
    if expected_size > MAX_RETAINED_PACKAGE_BYTES {
        return Err("the retained package exceeds the local save limit".to_owned());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "the selected package destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve package destination directory: {error}"))?;
    if !parent.is_dir() {
        return Err("the selected package destination directory is unavailable".to_owned());
    }
    let filename = destination
        .file_name()
        .ok_or_else(|| "the selected package destination has no filename".to_owned())?;
    let destination = parent.join(filename);
    let temporary = parent.join(format!(
        ".{}.axkdeck-download-{}",
        filename.to_string_lossy(),
        candidate_id()?
    ));
    let mut output = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temporary)
        .map_err(|error| format!("create package download staging file: {error}"))?;
    let download_result = (|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let mut url = url::Url::parse(&connection.base_url)
            .map_err(|error| format!("parse axklib-server URL: {error}"))?;
        url.set_path(&content_path);
        url.set_query(None);
        url.set_fragment(None);
        let mut response = reqwest::blocking::Client::new()
            .get(url)
            .bearer_auth(connection.bearer_token)
            .send()
            .map_err(|error| format!("download package: {error}"))?
            .error_for_status()
            .map_err(|error| format!("download package: {error}"))?;
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut written = 0_u64;
        loop {
            let count = response
                .read(&mut buffer)
                .map_err(|error| format!("read package download: {error}"))?;
            if count == 0 {
                break;
            }
            written = written
                .checked_add(count as u64)
                .ok_or_else(|| "package download size overflow".to_owned())?;
            if written > expected_size {
                return Err("package download exceeded its declared size".to_owned());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("write package download: {error}"))?;
        }
        if written != expected_size {
            return Err("package download ended before its declared size".to_owned());
        }
        output
            .sync_all()
            .map_err(|error| format!("flush package download: {error}"))?;
        drop(output);
        Ok(())
    })();
    if let Err(error) = download_result {
        let _ = std::fs::remove_file(&temporary);
        return Err(error);
    }
    publish_downloaded_file(&temporary, &destination).map_err(|error| {
        format!(
            "{error}; the complete downloaded package remains available at {}",
            temporary.display()
        )
    })
}

fn parse_tar_octal(field: &[u8]) -> Result<u64, String> {
    let text = std::str::from_utf8(field)
        .map_err(|_| "SFZ archive contains a non-ASCII numeric field".to_owned())?
        .trim_matches(['\0', ' ']);
    if text.is_empty() {
        return Ok(0);
    }
    u64::from_str_radix(text, 8)
        .map_err(|_| "SFZ archive contains an invalid numeric field".to_owned())
}

fn checked_tar_path(header: &[u8; 512]) -> Result<PathBuf, String> {
    let field = |range: std::ops::Range<usize>| {
        let bytes = &header[range];
        &bytes[..bytes
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(bytes.len())]
    };
    let name = std::str::from_utf8(field(0..100))
        .map_err(|_| "SFZ archive path is not valid UTF-8".to_owned())?;
    let prefix = std::str::from_utf8(field(345..500))
        .map_err(|_| "SFZ archive path is not valid UTF-8".to_owned())?;
    let value = if prefix.is_empty() {
        name.to_owned()
    } else {
        format!("{prefix}/{name}")
    };
    let path = PathBuf::from(value);
    if path.as_os_str().is_empty()
        || path.is_absolute()
        || path
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err("SFZ archive contains an unsafe path".to_owned());
    }
    Ok(path)
}

fn verify_tar_checksum(header: &[u8; 512]) -> Result<(), String> {
    let expected = parse_tar_octal(&header[148..156])?;
    let actual = header
        .iter()
        .enumerate()
        .map(|(index, byte)| {
            if (148..156).contains(&index) {
                u64::from(b' ')
            } else {
                u64::from(*byte)
            }
        })
        .sum::<u64>();
    if actual != expected {
        return Err("SFZ archive header checksum is invalid".to_owned());
    }
    Ok(())
}

fn extract_sfz_tar(archive: &mut File, destination: &Path) -> Result<(), String> {
    let parent = destination
        .parent()
        .ok_or_else(|| "the SFZ destination has no parent directory".to_owned())?;
    if destination.exists() {
        return Err("the selected SFZ export folder already exists".to_owned());
    }
    let name = destination
        .file_name()
        .ok_or_else(|| "the SFZ destination has no folder name".to_owned())?;
    let staging = parent.join(format!(
        ".{}.axkdeck-extract-{}",
        name.to_string_lossy(),
        candidate_id()?
    ));
    std::fs::create_dir(&staging)
        .map_err(|error| format!("create SFZ extraction staging folder: {error}"))?;
    let result = (|| {
        archive
            .seek(SeekFrom::Start(0))
            .map_err(|error| format!("rewind SFZ archive: {error}"))?;
        let mut entries = 0_usize;
        let mut zero_blocks = 0_u8;
        loop {
            let mut header = [0_u8; 512];
            archive
                .read_exact(&mut header)
                .map_err(|error| format!("read SFZ archive header: {error}"))?;
            if header.iter().all(|byte| *byte == 0) {
                zero_blocks += 1;
                if zero_blocks == 2 {
                    break;
                }
                continue;
            }
            if zero_blocks != 0 {
                return Err("SFZ archive has a malformed end marker".to_owned());
            }
            entries += 1;
            if entries > MAX_RETAINED_SFZ_EXPORT_ENTRIES {
                return Err("SFZ archive contains too many entries".to_owned());
            }
            verify_tar_checksum(&header)?;
            if &header[257..263] != b"ustar\0" {
                return Err("SFZ archive is not in the supported USTAR profile".to_owned());
            }
            let relative = checked_tar_path(&header)?;
            let output_path = staging.join(relative);
            let size = parse_tar_octal(&header[124..136])?;
            match header[156] {
                b'5' => {
                    if size != 0 {
                        return Err("SFZ archive directory has an invalid payload".to_owned());
                    }
                    std::fs::create_dir_all(&output_path)
                        .map_err(|error| format!("create SFZ export directory: {error}"))?;
                }
                0 | b'0' => {
                    if let Some(directory) = output_path.parent() {
                        std::fs::create_dir_all(directory)
                            .map_err(|error| format!("create SFZ export directory: {error}"))?;
                    }
                    let mut output = OpenOptions::new()
                        .create_new(true)
                        .write(true)
                        .open(&output_path)
                        .map_err(|error| format!("create SFZ export file: {error}"))?;
                    let mut remaining = size;
                    let mut buffer = vec![0_u8; 1024 * 1024];
                    while remaining != 0 {
                        let count = usize::try_from(remaining.min(buffer.len() as u64))
                            .map_err(|_| "SFZ archive entry size is unsupported".to_owned())?;
                        archive
                            .read_exact(&mut buffer[..count])
                            .map_err(|error| format!("read SFZ archive payload: {error}"))?;
                        output
                            .write_all(&buffer[..count])
                            .map_err(|error| format!("write SFZ export file: {error}"))?;
                        remaining -= count as u64;
                    }
                    output
                        .sync_all()
                        .map_err(|error| format!("flush SFZ export file: {error}"))?;
                    let padding = (512 - size % 512) % 512;
                    archive
                        .seek(SeekFrom::Current(i64::try_from(padding).map_err(|_| {
                            "SFZ archive padding is unsupported".to_owned()
                        })?))
                        .map_err(|error| format!("skip SFZ archive padding: {error}"))?;
                }
                _ => return Err("SFZ archive contains an unsupported entry type".to_owned()),
            }
        }
        if destination.exists() {
            return Err("the selected SFZ export folder was created by another process".to_owned());
        }
        std::fs::rename(&staging, destination)
            .map_err(|error| format!("publish SFZ export folder: {error}"))
    })();
    if result.is_err() {
        let _ = std::fs::remove_dir_all(&staging);
    }
    result
}

fn download_retained_sfz_export(
    connection: server_sidecar::FrontendConnection,
    destination: PathBuf,
    content_path: String,
    expected_size: u64,
) -> Result<(), String> {
    if !valid_retained_content_path(&content_path) {
        return Err("the retained SFZ export path is invalid".to_owned());
    }
    if expected_size > MAX_RETAINED_SFZ_EXPORT_BYTES {
        return Err("the retained SFZ export exceeds the local save limit".to_owned());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "the SFZ destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve SFZ destination directory: {error}"))?;
    let destination = parent.join(
        destination
            .file_name()
            .ok_or_else(|| "the SFZ destination has no folder name".to_owned())?,
    );
    if destination.exists() {
        return Err("the selected SFZ export folder already exists".to_owned());
    }
    let temporary = parent.join(format!(".axkdeck-sfz-download-{}.tar", candidate_id()?));
    let mut output = OpenOptions::new()
        .create_new(true)
        .read(true)
        .write(true)
        .open(&temporary)
        .map_err(|error| format!("create SFZ download staging file: {error}"))?;
    let result = (|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let mut url = url::Url::parse(&connection.base_url)
            .map_err(|error| format!("parse axklib-server URL: {error}"))?;
        url.set_path(&content_path);
        url.set_query(None);
        url.set_fragment(None);
        let mut response = reqwest::blocking::Client::new()
            .get(url)
            .bearer_auth(connection.bearer_token)
            .send()
            .map_err(|error| format!("download SFZ export: {error}"))?
            .error_for_status()
            .map_err(|error| format!("download SFZ export: {error}"))?;
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut written = 0_u64;
        loop {
            let count = response
                .read(&mut buffer)
                .map_err(|error| format!("read SFZ export download: {error}"))?;
            if count == 0 {
                break;
            }
            written = written
                .checked_add(count as u64)
                .ok_or_else(|| "SFZ export download size overflow".to_owned())?;
            if written > expected_size {
                return Err("SFZ export download exceeded its declared size".to_owned());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("write SFZ export download: {error}"))?;
        }
        if written != expected_size {
            return Err("SFZ export download ended before its declared size".to_owned());
        }
        output
            .sync_all()
            .map_err(|error| format!("flush SFZ export download: {error}"))?;
        extract_sfz_tar(&mut output, &destination)
    })();
    drop(output);
    let _ = std::fs::remove_file(&temporary);
    result
}

#[tauri::command]
async fn save_retained_package(
    candidate_id: String,
    content_path: String,
    expected_size: u64,
    candidates: State<'_, Mutex<PackageSaveCandidateStore>>,
    connections: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<(), String> {
    let destination = candidates
        .lock()
        .map_err(|_| "package destination state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "package destination expired; choose it again".to_owned())?;
    let connection = connections
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .connection()?
        .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
    tauri::async_runtime::spawn_blocking(move || {
        download_retained_package(connection, destination, content_path, expected_size)
    })
    .await
    .map_err(|error| format!("save package worker failed: {error}"))?
}

#[tauri::command]
async fn save_retained_sfz_export(
    candidate_id: String,
    content_path: String,
    expected_size: u64,
    candidates: State<'_, Mutex<SfzSaveCandidateStore>>,
    connections: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<(), String> {
    let destination = candidates
        .lock()
        .map_err(|_| "SFZ destination state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "SFZ destination expired; choose it again".to_owned())?;
    let connection = connections
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .connection()?
        .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
    tauri::async_runtime::spawn_blocking(move || {
        download_retained_sfz_export(connection, destination, content_path, expected_size)
    })
    .await
    .map_err(|error| format!("save SFZ export worker failed: {error}"))?
}

#[tauri::command]
async fn select_local_workspace(
    app: AppHandle,
    window: WebviewWindow,
    state: State<'_, Mutex<WorkspaceCandidateStore>>,
) -> Result<Option<WorkspaceCandidate>, String> {
    log::info!("opening native workspace folder picker");
    let selected = tauri::async_runtime::spawn_blocking(move || {
        app.dialog()
            .file()
            .set_title("Choose workspace directory")
            .set_parent(&window)
            .blocking_pick_folder()
    })
    .await
    .map_err(|error| format!("open workspace folder picker: {error}"))?;
    let Some(selected) = selected else {
        log::info!("native workspace folder picker was cancelled");
        return Ok(None);
    };
    let path = selected
        .into_path()
        .map_err(|_| "the selected folder is not a local filesystem path".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve selected workspace: {error}"))?;
    if !path.is_dir() {
        return Err("the selected workspace is not a directory".to_owned());
    }
    let suggested_name = path
        .file_name()
        .and_then(|value| value.to_str())
        .filter(|value| !value.is_empty())
        .unwrap_or("Workspace")
        .to_owned();
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "system clock is unavailable".to_owned())?
        .as_nanos();
    let candidate_id = format!("{}-{nonce:x}", std::process::id());
    let mut candidates = state
        .lock()
        .map_err(|_| "workspace selection state is unavailable".to_owned())?;
    candidates
        .values
        .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(300));
    candidates
        .values
        .insert(candidate_id.clone(), (path, Instant::now()));
    log::info!("native workspace directory selected; awaiting confirmation");
    Ok(Some(WorkspaceCandidate {
        candidate_id,
        suggested_name,
    }))
}

#[tauri::command]
fn commit_local_workspace(
    candidate_id: String,
    display_name: String,
    writable: bool,
    revision: u64,
    candidates: State<'_, Mutex<WorkspaceCandidateStore>>,
    connections: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<(), String> {
    if display_name.trim().is_empty() {
        return Err("enter a workspace name".to_owned());
    }
    let path = candidates
        .lock()
        .map_err(|_| "workspace selection state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "workspace selection expired; choose the folder again".to_owned())?;
    let connection = connections
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .connection()?
        .ok_or_else(|| "local axklib-server is unavailable".to_owned())?;
    server_sidecar::create_workspace(&connection, &path, display_name.trim(), writable, revision)?;
    log::info!("local workspace committed: {display_name}");
    Ok(())
}

#[tauri::command]
fn open_developer_tools(window: WebviewWindow) -> Result<(), String> {
    #[cfg(debug_assertions)]
    {
        window.open_devtools();
        Ok(())
    }
    #[cfg(not(debug_assertions))]
    {
        let _ = window;
        Err("developer tools are available only in development builds".to_owned())
    }
}

#[tauri::command]
fn diagnostic_log_level() -> &'static str {
    log_level_name(configured_log_level())
}

#[tauri::command]
fn desktop_build_info() -> DesktopBuildInfo {
    current_build_info()
}

#[tauri::command]
fn server_connection(
    state: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<Option<server_sidecar::FrontendConnection>, String> {
    state
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .connection()
}

#[tauri::command]
fn remote_server_settings(
    state: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<remote_settings::RemoteServerSettingsView, String> {
    state
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())
        .map(|manager| manager.settings())
}

#[tauri::command]
fn validate_remote_server_settings(
    settings: remote_settings::RemoteServerSettingsInput,
) -> Result<server_sidecar::FrontendConnection, String> {
    remote_settings::validate_remote_connection(settings)
}

#[tauri::command]
fn configure_remote_server(
    settings: remote_settings::RemoteServerSettingsInput,
    state: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<remote_settings::RemoteServerSettingsView, String> {
    state
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .configure_remote(settings)
}

#[tauri::command]
fn use_local_server(
    state: State<'_, Mutex<remote_settings::ServerConnectionManager>>,
) -> Result<remote_settings::RemoteServerSettingsView, String> {
    state
        .lock()
        .map_err(|_| "server connection settings are unavailable".to_owned())?
        .use_local()
}

#[cfg(target_os = "linux")]
fn configure_linux_webkit() {
    let dmabuf_requested = std::env::var("AXKDECK_ENABLE_DMABUF").is_ok_and(|value| {
        matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "1" | "true" | "yes"
        )
    });

    if !dmabuf_requested {
        // SAFETY: This runs before Tauri, WebKitGTK, or application workers.
        unsafe {
            std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    #[cfg(target_os = "linux")]
    configure_linux_webkit();

    let log_level = configured_log_level();
    let log_targets = vec![
        Target::new(TargetKind::LogDir {
            file_name: Some("axkdeck".to_owned()),
        }),
        #[cfg(debug_assertions)]
        Target::new(TargetKind::Stdout),
    ];
    let log_plugin = tauri_plugin_log::Builder::new()
        .targets(log_targets)
        .level(log_level)
        .max_file_size(LOG_FILE_SIZE)
        .rotation_strategy(RotationStrategy::KeepSome(RETAINED_LOG_FILES))
        .build();

    tauri::Builder::default()
        .plugin(log_plugin)
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .manage(Mutex::new(WorkspaceCandidateStore {
            values: HashMap::new(),
        }))
        .manage(Mutex::new(PackageSaveCandidateStore {
            values: HashMap::new(),
        }))
        .manage(Mutex::new(SfzSaveCandidateStore {
            values: HashMap::new(),
        }))
        .setup(|app| {
            let log_directory = app
                .path()
                .app_log_dir()
                .map_err(|error| format!("resolve application log directory: {error}"))?;
            let application_data_directory = app
                .path()
                .app_local_data_dir()
                .map_err(|error| format!("resolve application state directory: {error}"))?;
            let preferences_path = application_data_directory.join("desktop-preferences.json");
            let preferences = DesktopPreferencesStore::load(preferences_path.clone()).unwrap_or_else(|error| {
                log::warn!("desktop preferences are unavailable and will be reset on the next update: {error}");
                DesktopPreferencesStore::empty(preferences_path)
            });
            app.manage(Mutex::new(preferences));
            let state_directory = application_data_directory.join("server-state");
            let manager = remote_settings::ServerConnectionManager::initialize(
                log_directory.clone(),
                state_directory.clone(),
            )
            .unwrap_or_else(|error| {
                log::error!("local axklib-server initialization failed: {error}");
                remote_settings::ServerConnectionManager::unavailable(
                    error,
                    log_directory,
                    state_directory,
                )
            });
            app.manage(Mutex::new(manager));
            let build = current_build_info();
            log::info!(
                "axkdeck desktop shell initialized: version={} source={}",
                build.semantic_version,
                build.source_identity
            );
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            server_connection,
            remote_server_settings,
            validate_remote_server_settings,
            configure_remote_server,
            use_local_server,
            select_local_workspace,
            commit_local_workspace,
            select_local_package,
            select_local_package_destination,
            save_retained_package,
            select_local_sfz_destination,
            save_retained_sfz_export,
            open_developer_tools,
            diagnostic_log_level,
            desktop_build_info
        ])
        .run(tauri::generate_context!())
        .expect("failed to run axkdeck");
}
