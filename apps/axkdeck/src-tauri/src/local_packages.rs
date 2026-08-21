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
const SUPPORTED_PACKAGE_EXTENSIONS: [&str; 8] = [
    "a3k", "axkvol", "axkprg", "axksbac", "axksbnk", "axksmpl", "axkseq", "axkpkg",
];
const SUPPORTED_MEDIA_EXTENSIONS: [&str; 3] = ["iso", "ima", "zip"];

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

pub(crate) fn supported_package_extension(filename: &str) -> Option<&'static str> {
    let extension = Path::new(filename).extension()?.to_str()?;
    SUPPORTED_PACKAGE_EXTENSIONS
        .iter()
        .copied()
        .find(|supported| extension.eq_ignore_ascii_case(supported))
}

pub(crate) fn supported_media_extension(filename: &str) -> Option<&'static str> {
    let extension = Path::new(filename).extension()?.to_str()?;
    SUPPORTED_MEDIA_EXTENSIONS
        .iter()
        .copied()
        .find(|supported| extension.eq_ignore_ascii_case(supported))
}

pub(crate) fn package_picker_hint(
    preferred_path: Option<&str>,
) -> (Option<PathBuf>, Option<String>) {
    let Some(path) = preferred_path
        .filter(|value| !value.is_empty())
        .map(Path::new)
    else {
        return (None, None);
    };
    let directory = path
        .parent()
        .and_then(|parent| parent.canonicalize().ok())
        .filter(|parent| parent.is_dir());
    let filename = if directory.is_some() && path.is_file() {
        path.file_name()
            .and_then(|value| value.to_str())
            .filter(|value| supported_package_extension(value).is_some())
            .map(str::to_owned)
    } else {
        None
    };
    (directory, filename)
}

#[tauri::command]
pub(crate) async fn select_local_package(
    app: AppHandle,
    window: WebviewWindow,
    preferred_path: Option<String>,
) -> Result<Option<String>, String> {
    let scope_window = window.clone();
    let (starting_directory, starting_filename) = package_picker_hint(preferred_path.as_deref());
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Choose axklib package")
            .add_filter(
                "axklib packages and A3K archives",
                &SUPPORTED_PACKAGE_EXTENSIONS,
            )
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        if let Some(filename) = starting_filename {
            dialog = dialog.set_file_name(filename);
        }
        dialog.blocking_pick_file()
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
pub(crate) async fn select_local_packages(
    app: AppHandle,
    window: WebviewWindow,
    preferred_path: Option<String>,
) -> Result<Vec<String>, String> {
    let scope_window = window.clone();
    let (starting_directory, _) = package_picker_hint(preferred_path.as_deref());
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Choose axklib packages")
            .add_filter(
                "axklib packages and A3K archives",
                &SUPPORTED_PACKAGE_EXTENSIONS,
            )
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_pick_files()
    })
    .await
    .map_err(|error| format!("open package file picker: {error}"))?;
    selected
        .unwrap_or_default()
        .into_iter()
        .map(|file| {
            let path = file
                .into_path()
                .map_err(|_| "the selected package is not a local filesystem path".to_owned())?
                .canonicalize()
                .map_err(|error| format!("resolve selected package: {error}"))?;
            if !path.is_file()
                || path
                    .file_name()
                    .and_then(|value| value.to_str())
                    .and_then(supported_package_extension)
                    .is_none()
            {
                return Err(
                    "every selected file must be a supported axklib package or A3K archive"
                        .to_owned(),
                );
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
        .collect()
}

#[tauri::command]
pub(crate) async fn select_local_package_destination(
    app: AppHandle,
    window: WebviewWindow,
    suggested_name: String,
    state: State<'_, Mutex<PackageSaveCandidateStore>>,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<PackageSaveCandidate>, String> {
    let expected_extension = supported_package_extension(&suggested_name)
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
pub(crate) async fn select_local_media_destination(
    app: AppHandle,
    window: WebviewWindow,
    suggested_name: String,
    state: State<'_, Mutex<PackageSaveCandidateStore>>,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<PackageSaveCandidate>, String> {
    let expected_extension = supported_media_extension(&suggested_name)
        .ok_or_else(|| "the suggested media filename has an unsupported extension".to_owned())?
        .to_owned();
    let picker_extension = expected_extension.clone();
    let starting_directory = match preferences.lock() {
        Ok(preferences) => preferences.media_export_directory(),
        Err(_) => {
            log::warn!("desktop preference state is unavailable; using the platform save location");
            None
        }
    };
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Save sampler media export")
            .add_filter("sampler media export", &[picker_extension.as_str()])
            .set_file_name(suggested_name)
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_save_file()
    })
    .await
    .map_err(|error| format!("open media image save picker: {error}"))?;
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
                if let Err(error) = preferences.remember_media_export_directory(directory) {
                    log::warn!("could not persist the media export directory: {error}");
                }
            }
            Err(_) => {
                log::warn!(
                    "desktop preference state is unavailable; the media export directory was not retained"
                );
            }
        }
    }
    let candidate_id = candidate_id()?;
    let mut candidates = state
        .lock()
        .map_err(|_| "media destination state is unavailable".to_owned())?;
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

fn download_retained_file(
    connection: server_sidecar::FrontendConnection,
    destination: PathBuf,
    content_path: String,
    expected_size: u64,
) -> Result<(), String> {
    if !valid_retained_content_path(&content_path) {
        return Err("the retained download path is invalid".to_owned());
    }
    if expected_size > MAX_RETAINED_PACKAGE_BYTES {
        return Err("the retained download exceeds the local save limit".to_owned());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "the selected destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve destination directory: {error}"))?;
    if !parent.is_dir() {
        return Err("the selected destination directory is unavailable".to_owned());
    }
    let filename = destination
        .file_name()
        .ok_or_else(|| "the selected destination has no filename".to_owned())?;
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
        .map_err(|error| format!("create download staging file: {error}"))?;
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
            .map_err(|error| format!("download retained file: {error}"))?
            .error_for_status()
            .map_err(|error| format!("download retained file: {error}"))?;
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut written = 0_u64;
        loop {
            let count = response
                .read(&mut buffer)
                .map_err(|error| format!("read retained download: {error}"))?;
            if count == 0 {
                break;
            }
            written = written
                .checked_add(count as u64)
                .ok_or_else(|| "retained download size overflow".to_owned())?;
            if written > expected_size {
                return Err("retained download exceeded its declared size".to_owned());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("write retained download: {error}"))?;
        }
        if written != expected_size {
            return Err("retained download ended before its declared size".to_owned());
        }
        output
            .sync_all()
            .map_err(|error| format!("flush retained download: {error}"))?;
        drop(output);
        Ok(())
    })();
    if let Err(error) = download_result {
        let _ = std::fs::remove_file(&temporary);
        return Err(error);
    }
    publish_downloaded_file(&temporary, &destination).map_err(|error| {
        format!(
            "{error}; the complete downloaded file remains available at {}",
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
    connections: State<'_, remote_settings::ServerConnectionState>,
) -> Result<(), String> {
    let destination = candidates
        .lock()
        .map_err(|_| "package destination state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "package destination expired; choose it again".to_owned())?;
    let connections = connections.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let connection = connections
            .connection()?
            .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
        download_retained_file(connection, destination, content_path, expected_size)
    })
    .await
    .map_err(|error| format!("save package worker failed: {error}"))?
}

#[tauri::command]
pub(crate) async fn save_retained_media(
    candidate_id: String,
    content_path: String,
    expected_size: u64,
    candidates: State<'_, Mutex<PackageSaveCandidateStore>>,
    connections: State<'_, remote_settings::ServerConnectionState>,
) -> Result<(), String> {
    let destination = candidates
        .lock()
        .map_err(|_| "media destination state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "media destination expired; choose it again".to_owned())?;
    let connections = connections.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let connection = connections
            .connection()?
            .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
        download_retained_file(connection, destination, content_path, expected_size)
    })
    .await
    .map_err(|error| format!("save media image worker failed: {error}"))?
}
