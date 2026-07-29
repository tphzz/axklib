use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde::Serialize;
use tauri::{AppHandle, Manager, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_fs::FsExt;

use crate::desktop_preferences::DesktopPreferencesStore;
use crate::{file_publication, remote_settings, retained_download, server_sidecar};

const MAX_RETAINED_PACKAGE_BYTES: u64 = 4 * 1024 * 1024 * 1024;

#[derive(Default)]
pub(crate) struct PackageSaveCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PackageSaveCandidate {
    candidate_id: String,
    filename: String,
}

pub(crate) fn candidate_id() -> Result<String, String> {
    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| "system clock is unavailable".to_owned())?
        .as_nanos();
    Ok(format!("{}-{nonce:x}", std::process::id()))
}

pub(crate) fn valid_retained_content_path(path: &str) -> bool {
    path.strip_prefix("/api/v1/download-archives/")
        .and_then(|value| value.strip_suffix("/content"))
        .is_some_and(|archive_id| {
            !archive_id.is_empty() && archive_id.bytes().all(|byte| byte.is_ascii_alphanumeric())
        })
}

pub(crate) fn normalize_package_destination(
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

#[tauri::command]
pub(crate) async fn select_local_package(
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
pub(crate) async fn select_local_package_destination(
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
        let mut response = retained_download::client()?
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

#[tauri::command]
pub(crate) async fn save_retained_package(
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
