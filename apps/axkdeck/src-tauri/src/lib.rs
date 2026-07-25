mod remote_settings;
mod server_sidecar;

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;
use tauri::{AppHandle, Manager, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_fs::FsExt;
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};

const LOG_FILE_SIZE: u128 = 5 * 1024 * 1024;
const RETAINED_LOG_FILES: usize = 3;
const MAX_RETAINED_PACKAGE_BYTES: u64 = 4 * 1024 * 1024 * 1024;

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
        current_build_info, normalize_package_destination, parse_log_level,
        valid_retained_content_path,
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
}

struct WorkspaceCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

struct PackageSaveCandidateStore {
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
    let selected = tauri::async_runtime::spawn_blocking(move || {
        app.dialog()
            .file()
            .set_title("Save axklib package")
            .add_filter("axklib package", &[picker_extension.as_str()])
            .set_file_name(suggested_name)
            .set_parent(&window)
            .blocking_save_file()
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

fn publish_downloaded_file(temporary: &Path, destination: &Path) -> Result<(), String> {
    if !destination.exists() {
        return std::fs::rename(temporary, destination)
            .map_err(|error| format!("publish downloaded package: {error}"));
    }
    if destination
        .symlink_metadata()
        .map_err(|error| format!("inspect package destination: {error}"))?
        .file_type()
        .is_symlink()
    {
        return Err("refusing to replace a symbolic-link destination".to_owned());
    }
    let backup = destination.with_file_name(format!(
        ".{}.axkdeck-backup-{}",
        destination
            .file_name()
            .and_then(|value| value.to_str())
            .unwrap_or("package"),
        candidate_id()?
    ));
    std::fs::rename(destination, &backup)
        .map_err(|error| format!("preserve existing package destination: {error}"))?;
    if let Err(error) = std::fs::rename(temporary, destination) {
        let _ = std::fs::rename(&backup, destination);
        return Err(format!("publish downloaded package: {error}"));
    }
    std::fs::remove_file(&backup)
        .map_err(|error| format!("remove replaced package backup: {error}"))
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
        publish_downloaded_file(&temporary, &destination)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
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
        .setup(|app| {
            let log_directory = app
                .path()
                .app_log_dir()
                .map_err(|error| format!("resolve application log directory: {error}"))?;
            let state_directory = app
                .path()
                .app_local_data_dir()
                .map_err(|error| format!("resolve application state directory: {error}"))?
                .join("server-state");
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
            open_developer_tools,
            diagnostic_log_level,
            desktop_build_info
        ])
        .run(tauri::generate_context!())
        .expect("failed to run axkdeck");
}
