use std::collections::HashMap;
use std::fs::OpenOptions;
use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use serde::Serialize;
use tauri::{AppHandle, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;

use crate::desktop_preferences::DesktopPreferencesStore;
use crate::directory_tar::{ExportControl, extract_directory_tar, receive_archive};
use crate::local_packages::{candidate_id, valid_retained_content_path};
use crate::{remote_settings, retained_download, server_sidecar};

#[cfg(test)]
#[path = "local_directory_exports_tests.rs"]
mod tests;

#[derive(Default)]
pub(crate) struct DirectorySaveCandidateStore {
    values: HashMap<String, SaveCandidate>,
}

struct SaveCandidate {
    destination: Option<PathBuf>,
    created: Instant,
    control: ExportControl,
}

impl DirectorySaveCandidateStore {
    fn insert(&mut self, id: String, destination: PathBuf) {
        self.values.retain(|_, candidate| {
            candidate.destination.is_none()
                || candidate.created.elapsed() < Duration::from_secs(300)
        });
        self.values.insert(
            id,
            SaveCandidate {
                destination: Some(destination),
                created: Instant::now(),
                control: ExportControl::default(),
            },
        );
    }

    fn begin(&mut self, id: &str) -> Result<(PathBuf, ExportControl), String> {
        let candidate = self
            .values
            .get_mut(id)
            .filter(|candidate| candidate.created.elapsed() < Duration::from_secs(300))
            .ok_or_else(|| "export destination expired; choose it again".to_owned())?;
        let destination = candidate
            .destination
            .take()
            .ok_or_else(|| "export destination is already in use".to_owned())?;
        Ok((destination, candidate.control.clone()))
    }

    fn cancel(&self, id: &str) {
        if let Some(candidate) = self.values.get(id) {
            candidate.control.cancelled.store(true, Ordering::Relaxed);
        }
    }

    fn finish(&mut self, id: &str) {
        self.values.remove(id);
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct DirectorySaveCandidate {
    candidate_id: String,
    directory_name: String,
}

pub(crate) fn normalize_directory_destination(path: PathBuf) -> Result<PathBuf, String> {
    let parent = path
        .parent()
        .filter(|value| !value.as_os_str().is_empty())
        .ok_or_else(|| "the selected export destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve export destination directory: {error}"))?;
    if !parent.is_dir() {
        return Err("the selected export destination parent is unavailable".to_owned());
    }
    let name = path
        .file_name()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| "the selected export destination has no folder name".to_owned())?;
    if name == "." || name == ".." {
        return Err("the selected export destination folder name is invalid".to_owned());
    }
    let destination = parent.join(name);
    if destination.exists() {
        return Err("the selected export folder already exists".to_owned());
    }
    Ok(destination)
}

#[tauri::command]
pub(crate) async fn select_local_directory_export_destination(
    app: AppHandle,
    window: WebviewWindow,
    suggested_name: String,
    export_label: String,
    state: State<'_, Mutex<DirectorySaveCandidateStore>>,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<DirectorySaveCandidate>, String> {
    if suggested_name.is_empty()
        || suggested_name == "."
        || suggested_name == ".."
        || suggested_name.contains('/')
        || suggested_name.contains('\\')
    {
        return Err("the suggested export folder name is invalid".to_owned());
    }
    if !matches!(
        export_label.as_str(),
        "SFZ" | "WAV" | "MIDI" | "Packages" | "Floppies" | "Files"
    ) {
        return Err("the directory export kind is unsupported".to_owned());
    }
    let starting_directory = match preferences.lock() {
        Ok(preferences) => preferences.directory_export_directory(),
        Err(_) => {
            log::warn!("axkdeck settings state is unavailable; using the platform save location");
            None
        }
    };
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title(format!("Save {export_label} export folder"))
            .set_file_name(suggested_name)
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_save_file()
    })
    .await
    .map_err(|error| format!("open directory export picker: {error}"))?;
    let Some(selected) = selected else {
        return Ok(None);
    };
    let destination = normalize_directory_destination(
        selected
            .into_path()
            .map_err(|_| "the selected destination is not a local filesystem path".to_owned())?,
    )?;
    let directory_name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "the selected export destination has no valid folder name".to_owned())?
        .to_owned();
    if let Some(directory) = destination.parent() {
        match preferences.lock() {
            Ok(mut preferences) => {
                if let Err(error) = preferences.remember_directory_export_directory(directory) {
                    log::warn!("could not persist the directory export location: {error}");
                }
            }
            Err(_) => {
                log::warn!(
                    "axkdeck settings state is unavailable; the directory export location was not retained"
                );
            }
        }
    }
    let candidate_id = candidate_id()?;
    let mut candidates = state
        .lock()
        .map_err(|_| "directory export destination state is unavailable".to_owned())?;
    candidates.insert(candidate_id.clone(), destination);
    Ok(Some(DirectorySaveCandidate {
        candidate_id,
        directory_name,
    }))
}

pub(crate) fn download_retained_directory_export(
    connection: server_sidecar::FrontendConnection,
    destination: PathBuf,
    content_path: String,
    expected_size: u64,
    control: &ExportControl,
) -> Result<(), String> {
    control.check()?;
    if !valid_retained_content_path(&content_path) {
        return Err("the retained directory export path is invalid".to_owned());
    }
    if expected_size > control.maximum_archive_bytes {
        return Err("the retained directory export exceeds the local save limit".to_owned());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "the export destination has no parent directory".to_owned())?
        .canonicalize()
        .map_err(|error| format!("resolve export destination directory: {error}"))?;
    let destination = parent.join(
        destination
            .file_name()
            .ok_or_else(|| "the export destination has no folder name".to_owned())?,
    );
    if destination.exists() {
        return Err("the selected export folder already exists".to_owned());
    }
    let temporary = parent.join(format!(
        ".axkdeck-directory-download-{}.tar",
        candidate_id()?
    ));
    let mut output = OpenOptions::new()
        .create_new(true)
        .read(true)
        .write(true)
        .open(&temporary)
        .map_err(|error| format!("create directory download staging file: {error}"))?;
    let result = (|| {
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
            .map_err(|error| format!("download directory export: {error}"))?
            .error_for_status()
            .map_err(|error| format!("download directory export: {error}"))?;
        receive_archive(&mut response, &mut output, expected_size, control)?;
        output
            .sync_all()
            .map_err(|error| format!("flush directory export download: {error}"))?;
        extract_directory_tar(&mut output, &destination, control)
    })();
    drop(output);
    let _ = std::fs::remove_file(&temporary);
    result
}

#[tauri::command]
pub(crate) async fn save_retained_directory_export(
    candidate_id: String,
    content_path: String,
    expected_size: u64,
    candidates: State<'_, Mutex<DirectorySaveCandidateStore>>,
    connections: State<'_, remote_settings::ServerConnectionState>,
) -> Result<(), String> {
    let (destination, control) = candidates
        .lock()
        .map_err(|_| "directory export destination state is unavailable".to_owned())?
        .begin(&candidate_id)?;
    let connections = connections.inner().clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let connection = connections
            .connection()?
            .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
        download_retained_directory_export(
            connection,
            destination,
            content_path,
            expected_size,
            &control,
        )
    })
    .await
    .map_err(|error| format!("save directory export worker failed: {error}"));
    candidates
        .lock()
        .map_err(|_| "directory export destination state is unavailable".to_owned())?
        .finish(&candidate_id);
    result?
}

#[tauri::command]
pub(crate) fn cancel_retained_directory_export(
    candidate_id: String,
    candidates: State<'_, Mutex<DirectorySaveCandidateStore>>,
) -> Result<(), String> {
    candidates
        .lock()
        .map_err(|_| "directory export destination state is unavailable".to_owned())?
        .cancel(&candidate_id);
    Ok(())
}
