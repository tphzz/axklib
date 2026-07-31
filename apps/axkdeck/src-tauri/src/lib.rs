mod desktop_preferences;
mod file_publication;
mod local_directory_exports;
mod local_packages;
mod local_workspaces;
mod remote_settings;
mod retained_download;
mod server_sidecar;

use std::sync::Mutex;

use serde::Serialize;
use tauri::{Manager, State, WebviewWindow};
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};

use desktop_preferences::DesktopPreferencesStore;
use local_directory_exports::{
    DirectorySaveCandidateStore, save_retained_directory_export,
    select_local_directory_export_destination,
};
use local_packages::{
    PackageSaveCandidateStore, save_retained_package, select_local_package,
    select_local_package_destination,
};
use local_workspaces::{WorkspaceCandidateStore, commit_local_workspace, select_local_workspace};

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
    use std::path::{Path, PathBuf};

    use super::{current_build_info, parse_log_level};
    use crate::local_directory_exports::{
        checked_tar_path, extract_directory_tar, normalize_directory_destination,
    };
    use crate::local_packages::{
        normalize_package_destination, supported_package_extension, valid_retained_content_path,
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
    fn package_picker_accepts_every_current_package_extension() {
        for extension in [
            "axkvol", "axkprg", "axksbac", "axksbnk", "axksmpl", "axkseq", "axkpkg",
        ] {
            assert_eq!(
                supported_package_extension(&format!("Package.{extension}")),
                Some(extension)
            );
        }
        assert_eq!(
            supported_package_extension("Sequence.AXKSEQ"),
            Some("axkseq")
        );
        assert_eq!(supported_package_extension("Sequence.zip"), None);
        assert_eq!(supported_package_extension("Sequence"), None);
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
    fn directory_destination_must_be_new_but_uses_an_existing_parent() {
        let root = std::env::temp_dir().join(format!(
            "axkdeck-directory-destination-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir(&root).expect("create test root");
        assert_eq!(
            normalize_directory_destination(root.join("Instrument"))
                .expect("normalize destination"),
            root.canonicalize()
                .expect("canonical root")
                .join("Instrument")
        );
        std::fs::create_dir(root.join("Existing")).expect("create existing destination");
        assert!(normalize_directory_destination(root.join("Existing")).is_err());
        std::fs::remove_dir_all(root).expect("remove test root");
    }

    #[test]
    fn directory_tar_extraction_rejects_traversal_and_publishes_a_new_folder() {
        let root = std::env::temp_dir().join(format!(
            "axkdeck-directory-extraction-{}",
            std::process::id()
        ));
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
        extract_directory_tar(&mut archive, &root.join("Instrument"))
            .expect("extract directory archive");
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
        .manage(Mutex::new(WorkspaceCandidateStore::default()))
        .manage(Mutex::new(PackageSaveCandidateStore::default()))
        .manage(Mutex::new(DirectorySaveCandidateStore::default()))
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
            select_local_directory_export_destination,
            save_retained_directory_export,
            open_developer_tools,
            diagnostic_log_level,
            desktop_build_info
        ])
        .run(tauri::generate_context!())
        .expect("failed to run axkdeck");
}
