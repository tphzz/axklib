mod remote_settings;
mod server_sidecar;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;
use tauri::{AppHandle, Manager, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};

const LOG_FILE_SIZE: u128 = 5 * 1024 * 1024;
const RETAINED_LOG_FILES: usize = 3;

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
    use super::{current_build_info, parse_log_level};

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
}

struct WorkspaceCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct WorkspaceCandidate {
    candidate_id: String,
    suggested_name: String,
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
        .setup(|app| {
            let log_directory = app
                .path()
                .app_log_dir()
                .map_err(|error| format!("resolve application log directory: {error}"))?;
            let manager =
                remote_settings::ServerConnectionManager::initialize(log_directory.clone())
                    .unwrap_or_else(|error| {
                        log::error!("local axklib-server initialization failed: {error}");
                        remote_settings::ServerConnectionManager::unavailable(error, log_directory)
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
            open_developer_tools,
            diagnostic_log_level,
            desktop_build_info
        ])
        .run(tauri::generate_context!())
        .expect("failed to run axkdeck");
}
