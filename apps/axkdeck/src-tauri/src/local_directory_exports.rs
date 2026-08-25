use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde::Serialize;
use tauri::{AppHandle, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;

use crate::desktop_preferences::DesktopPreferencesStore;
use crate::local_packages::{candidate_id, valid_retained_content_path};
use crate::{remote_settings, retained_download, server_sidecar};

const MAX_RETAINED_DIRECTORY_EXPORT_BYTES: u64 = 4 * 1024 * 1024 * 1024;
const MAX_RETAINED_DIRECTORY_EXPORT_ENTRIES: usize = 100_000;

#[derive(Default)]
pub(crate) struct DirectorySaveCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
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
        "SFZ" | "WAV" | "MIDI" | "Packages" | "Floppies"
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
    candidates
        .values
        .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(300));
    candidates
        .values
        .insert(candidate_id.clone(), (destination, Instant::now()));
    Ok(Some(DirectorySaveCandidate {
        candidate_id,
        directory_name,
    }))
}

fn parse_tar_octal(field: &[u8]) -> Result<u64, String> {
    let text = std::str::from_utf8(field)
        .map_err(|_| "export archive contains a non-ASCII numeric field".to_owned())?
        .trim_matches(['\0', ' ']);
    if text.is_empty() {
        return Ok(0);
    }
    u64::from_str_radix(text, 8)
        .map_err(|_| "export archive contains an invalid numeric field".to_owned())
}

pub(crate) fn checked_tar_path(header: &[u8; 512]) -> Result<PathBuf, String> {
    let field = |range: std::ops::Range<usize>| {
        let bytes = &header[range];
        &bytes[..bytes
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(bytes.len())]
    };
    let name = std::str::from_utf8(field(0..100))
        .map_err(|_| "export archive path is not valid UTF-8".to_owned())?;
    let prefix = std::str::from_utf8(field(345..500))
        .map_err(|_| "export archive path is not valid UTF-8".to_owned())?;
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
        return Err("export archive contains an unsafe path".to_owned());
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
        return Err("export archive header checksum is invalid".to_owned());
    }
    Ok(())
}

pub(crate) fn extract_directory_tar(archive: &mut File, destination: &Path) -> Result<(), String> {
    let parent = destination
        .parent()
        .ok_or_else(|| "the export destination has no parent directory".to_owned())?;
    if destination.exists() {
        return Err("the selected export folder already exists".to_owned());
    }
    let name = destination
        .file_name()
        .ok_or_else(|| "the export destination has no folder name".to_owned())?;
    let staging = parent.join(format!(
        ".{}.axkdeck-extract-{}",
        name.to_string_lossy(),
        candidate_id()?
    ));
    std::fs::create_dir(&staging)
        .map_err(|error| format!("create export extraction staging folder: {error}"))?;
    let result = (|| {
        archive
            .seek(SeekFrom::Start(0))
            .map_err(|error| format!("rewind export archive: {error}"))?;
        let mut entries = 0_usize;
        let mut zero_blocks = 0_u8;
        loop {
            let mut header = [0_u8; 512];
            archive
                .read_exact(&mut header)
                .map_err(|error| format!("read export archive header: {error}"))?;
            if header.iter().all(|byte| *byte == 0) {
                zero_blocks += 1;
                if zero_blocks == 2 {
                    break;
                }
                continue;
            }
            if zero_blocks != 0 {
                return Err("export archive has a malformed end marker".to_owned());
            }
            entries += 1;
            if entries > MAX_RETAINED_DIRECTORY_EXPORT_ENTRIES {
                return Err("export archive contains too many entries".to_owned());
            }
            verify_tar_checksum(&header)?;
            if &header[257..263] != b"ustar\0" {
                return Err("export archive is not in the supported USTAR profile".to_owned());
            }
            let relative = checked_tar_path(&header)?;
            let output_path = staging.join(relative);
            let size = parse_tar_octal(&header[124..136])?;
            match header[156] {
                b'5' => {
                    if size != 0 {
                        return Err("export archive directory has an invalid payload".to_owned());
                    }
                    std::fs::create_dir_all(&output_path)
                        .map_err(|error| format!("create export directory: {error}"))?;
                }
                0 | b'0' => {
                    if let Some(directory) = output_path.parent() {
                        std::fs::create_dir_all(directory)
                            .map_err(|error| format!("create export directory: {error}"))?;
                    }
                    let mut output = OpenOptions::new()
                        .create_new(true)
                        .write(true)
                        .open(&output_path)
                        .map_err(|error| format!("create export file: {error}"))?;
                    let mut remaining = size;
                    let mut buffer = vec![0_u8; 1024 * 1024];
                    while remaining != 0 {
                        let count = usize::try_from(remaining.min(buffer.len() as u64))
                            .map_err(|_| "export archive entry size is unsupported".to_owned())?;
                        archive
                            .read_exact(&mut buffer[..count])
                            .map_err(|error| format!("read export archive payload: {error}"))?;
                        output
                            .write_all(&buffer[..count])
                            .map_err(|error| format!("write export file: {error}"))?;
                        remaining -= count as u64;
                    }
                    output
                        .sync_all()
                        .map_err(|error| format!("flush export file: {error}"))?;
                    let padding = (512 - size % 512) % 512;
                    archive
                        .seek(SeekFrom::Current(i64::try_from(padding).map_err(|_| {
                            "export archive padding is unsupported".to_owned()
                        })?))
                        .map_err(|error| format!("skip export archive padding: {error}"))?;
                }
                _ => return Err("export archive contains an unsupported entry type".to_owned()),
            }
        }
        if destination.exists() {
            return Err("the selected export folder was created by another process".to_owned());
        }
        std::fs::rename(&staging, destination)
            .map_err(|error| format!("publish export folder: {error}"))
    })();
    if result.is_err() {
        let _ = std::fs::remove_dir_all(&staging);
    }
    result
}

fn download_retained_directory_export(
    connection: server_sidecar::FrontendConnection,
    destination: PathBuf,
    content_path: String,
    expected_size: u64,
) -> Result<(), String> {
    if !valid_retained_content_path(&content_path) {
        return Err("the retained directory export path is invalid".to_owned());
    }
    if expected_size > MAX_RETAINED_DIRECTORY_EXPORT_BYTES {
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
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut written = 0_u64;
        loop {
            let count = response
                .read(&mut buffer)
                .map_err(|error| format!("read directory export download: {error}"))?;
            if count == 0 {
                break;
            }
            written = written
                .checked_add(count as u64)
                .ok_or_else(|| "directory export download size overflow".to_owned())?;
            if written > expected_size {
                return Err("directory export download exceeded its declared size".to_owned());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("write directory export download: {error}"))?;
        }
        if written != expected_size {
            return Err("directory export download ended before its declared size".to_owned());
        }
        output
            .sync_all()
            .map_err(|error| format!("flush directory export download: {error}"))?;
        extract_directory_tar(&mut output, &destination)
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
    let destination = candidates
        .lock()
        .map_err(|_| "directory export destination state is unavailable".to_owned())?
        .values
        .remove(&candidate_id)
        .filter(|(_, created)| created.elapsed() < Duration::from_secs(300))
        .map(|(path, _)| path)
        .ok_or_else(|| "export destination expired; choose it again".to_owned())?;
    let connections = connections.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let connection = connections
            .connection()?
            .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
        download_retained_directory_export(connection, destination, content_path, expected_size)
    })
    .await
    .map_err(|error| format!("save directory export worker failed: {error}"))?
}
