use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;
use tauri::{AppHandle, State, WebviewWindow};
use tauri_plugin_dialog::DialogExt;

use crate::{remote_settings, server_sidecar};

#[derive(Default)]
pub(crate) struct WorkspaceCandidateStore {
    values: HashMap<String, (PathBuf, Instant)>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct WorkspaceCandidate {
    candidate_id: String,
    suggested_name: String,
}

#[tauri::command]
pub(crate) async fn select_local_workspace(
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
pub(crate) fn commit_local_workspace(
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
