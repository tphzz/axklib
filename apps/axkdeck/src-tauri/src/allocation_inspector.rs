use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use tauri::{AppHandle, Manager, State, WebviewUrl, WebviewWindow, WebviewWindowBuilder};
use tauri_plugin_dialog::DialogExt;

use crate::desktop_preferences::DesktopPreferencesStore;

const WINDOW_LABEL: &str = "allocation-inspector";

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct AllocationInspectorRequest {
    image_id: String,
    revision: u64,
    partition_index: u32,
    partition_name: String,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct AllocationMapSaveRequest {
    suggested_name: String,
    document: serde_json::Value,
}

fn inspector_path(request: &AllocationInspectorRequest) -> PathBuf {
    let query = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("view", "allocation")
        .append_pair("imageId", &request.image_id)
        .append_pair("revision", &request.revision.to_string())
        .append_pair("partitionIndex", &request.partition_index.to_string())
        .append_pair("partitionName", &request.partition_name)
        .finish();
    PathBuf::from(format!("index.html?{query}"))
}

#[tauri::command]
pub(crate) async fn open_allocation_inspector(
    app: AppHandle,
    request: AllocationInspectorRequest,
) -> Result<(), String> {
    if let Some(existing) = app.get_webview_window(WINDOW_LABEL) {
        existing.destroy().map_err(|error| error.to_string())?;
    }
    WebviewWindowBuilder::new(
        &app,
        WINDOW_LABEL,
        WebviewUrl::App(inspector_path(&request)),
    )
    .title(format!("{} allocation", request.partition_name))
    .inner_size(1280.0, 800.0)
    .min_inner_size(760.0, 520.0)
    .visible(false)
    .build()
    .map_err(|error| error.to_string())?;
    Ok(())
}

fn normalize_json_destination(mut path: PathBuf) -> Result<PathBuf, String> {
    match path.extension().and_then(|value| value.to_str()) {
        None => {
            path.set_extension("json");
        }
        Some(extension) if extension.eq_ignore_ascii_case("json") => {}
        Some(_) => return Err("the allocation map destination must end in .json".to_owned()),
    }
    Ok(path)
}

fn encoded_allocation_document(document: &serde_json::Value) -> Result<Vec<u8>, String> {
    let mut encoded = serde_json::to_vec_pretty(document)
        .map_err(|error| format!("encode allocation map: {error}"))?;
    encoded.push(b'\n');
    Ok(encoded)
}

fn temporary_sibling(destination: &Path) -> Result<(PathBuf, File), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "allocation map destination has no parent directory".to_owned())?;
    let name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("allocation-map.json");
    for attempt in 0_u8..16 {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| "system clock is unavailable".to_owned())?
            .as_nanos();
        let path = parent.join(format!(
            ".{name}.{}-{nonce}-{attempt}.tmp",
            std::process::id()
        ));
        match OpenOptions::new().create_new(true).write(true).open(&path) {
            Ok(file) => return Ok((path, file)),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(format!("create allocation map staging file: {error}")),
        }
    }
    Err("could not reserve an allocation map staging file".to_owned())
}

fn write_allocation_document(
    destination: &Path,
    document: &serde_json::Value,
) -> Result<(), String> {
    let bytes = encoded_allocation_document(document)?;
    let (temporary, mut output) = temporary_sibling(destination)?;
    let result = (|| {
        output
            .write_all(&bytes)
            .map_err(|error| format!("write allocation map: {error}"))?;
        output
            .sync_all()
            .map_err(|error| format!("flush allocation map: {error}"))?;
        drop(output);
        let outcome = crate::file_publication::publish_file(&temporary, destination)?;
        if let Some(warning) = outcome.warning {
            log::warn!("{warning}");
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
    result
}

#[tauri::command]
pub(crate) async fn save_allocation_map_json(
    app: AppHandle,
    window: WebviewWindow,
    request: AllocationMapSaveRequest,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<Option<String>, String> {
    let suggested_path = Path::new(&request.suggested_name);
    if suggested_path.file_name() != Some(std::ffi::OsStr::new(&request.suggested_name)) {
        return Err(
            "the suggested allocation map filename must not contain a directory".to_owned(),
        );
    }
    normalize_json_destination(suggested_path.to_path_buf())?;
    let starting_directory = match preferences.lock() {
        Ok(preferences) => preferences.allocation_export_directory(),
        Err(_) => {
            log::warn!("axkdeck settings state is unavailable; using the platform save location");
            None
        }
    };
    let suggested_name = request.suggested_name;
    let selected = tauri::async_runtime::spawn_blocking(move || {
        let mut dialog = app
            .dialog()
            .file()
            .set_title("Export partition allocation map")
            .add_filter("JSON document", &["json"])
            .set_file_name(suggested_name)
            .set_parent(&window);
        if let Some(directory) = starting_directory {
            dialog = dialog.set_directory(directory);
        }
        dialog.blocking_save_file()
    })
    .await
    .map_err(|error| format!("open allocation map save picker: {error}"))?;
    let Some(selected) = selected else {
        return Ok(None);
    };
    let destination = selected
        .into_path()
        .map_err(|_| "the selected destination is not a local filesystem path".to_owned())?;
    let destination = normalize_json_destination(destination)?;
    let parent = destination
        .parent()
        .ok_or_else(|| "the allocation map destination has no parent directory".to_owned())?;
    match preferences.lock() {
        Ok(mut preferences) => {
            if let Err(error) = preferences.remember_allocation_export_directory(parent) {
                log::warn!("could not persist the allocation export directory: {error}");
            }
        }
        Err(_) => {
            log::warn!(
                "axkdeck settings state is unavailable; the allocation export directory was not retained"
            );
        }
    }
    let result_path = destination.to_string_lossy().into_owned();
    let document = request.document;
    tauri::async_runtime::spawn_blocking(move || {
        write_allocation_document(&destination, &document)
    })
    .await
    .map_err(|error| format!("save allocation map: {error}"))??;
    Ok(Some(result_path))
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::{
        AllocationInspectorRequest, encoded_allocation_document, inspector_path,
        normalize_json_destination, write_allocation_document,
    };

    #[test]
    fn inspector_path_encodes_revision_pinned_partition_identity() {
        let path = inspector_path(&AllocationInspectorRequest {
            image_id: "image/one".to_string(),
            revision: 7,
            partition_index: 2,
            partition_name: "Sounds & Tests".to_string(),
        });
        assert_eq!(
            path.to_string_lossy(),
            "index.html?view=allocation&imageId=image%2Fone&revision=7&partitionIndex=2&partitionName=Sounds+%26+Tests"
        );
    }

    #[test]
    fn allocation_json_destination_requires_json_extension() {
        assert_eq!(
            normalize_json_destination("allocation".into()).expect("append extension"),
            PathBuf::from("allocation.json")
        );
        assert_eq!(
            normalize_json_destination("allocation.JSON".into()).expect("accept extension"),
            PathBuf::from("allocation.JSON")
        );
        assert!(normalize_json_destination("allocation.txt".into()).is_err());
    }

    #[test]
    fn allocation_document_is_pretty_printed_with_final_newline() {
        let encoded = encoded_allocation_document(&serde_json::json!({"partitionName": "Sounds"}))
            .expect("encode document");
        assert_eq!(
            String::from_utf8(encoded).expect("utf-8"),
            "{\n  \"partitionName\": \"Sounds\"\n}\n"
        );
    }

    #[test]
    fn allocation_document_replaces_existing_destination() {
        let root = std::env::temp_dir().join(format!(
            "axkdeck-allocation-export-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("system clock")
                .as_nanos()
        ));
        fs::create_dir(&root).expect("create temporary directory");
        let destination = root.join("allocation.json");
        fs::write(&destination, b"old").expect("write old destination");

        write_allocation_document(&destination, &serde_json::json!({"freeClusters": 10}))
            .expect("write allocation document");
        assert_eq!(
            fs::read_to_string(&destination).expect("read allocation document"),
            "{\n  \"freeClusters\": 10\n}\n"
        );
        fs::remove_dir_all(root).expect("remove temporary directory");
    }
}
