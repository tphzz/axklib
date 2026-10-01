mod export;
mod files;
mod pages;
mod reader;
mod records;

use pages::{Page, ReadRequest, read_page};
use reader::LogReader;
use records::Filter;
use serde::Serialize;
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager, State, WebviewUrl, WebviewWindow, WebviewWindowBuilder};
use tauri_plugin_dialog::DialogExt;

pub const WINDOW_LABEL: &str = "logs";

#[derive(Clone)]
pub struct LogState(Arc<Mutex<LogReader>>);

impl LogState {
    pub fn new(directory: std::path::PathBuf) -> Self {
        Self(Arc::new(Mutex::new(LogReader::new(directory))))
    }
}

fn require_viewer(window: &WebviewWindow) -> Result<(), String> {
    if window.label() == WINDOW_LABEL {
        Ok(())
    } else {
        Err("This command is only available in the Logs window".into())
    }
}

#[tauri::command]
pub async fn open_diagnostic_logs(app: AppHandle, window: WebviewWindow) -> Result<(), String> {
    if window.label() != "main" && window.label() != WINDOW_LABEL {
        return Err("Logs can only be opened from the main window".into());
    }
    if let Some(existing) = app.get_webview_window(WINDOW_LABEL) {
        existing.unminimize().map_err(|e| e.to_string())?;
        existing.show().map_err(|e| e.to_string())?;
        existing.set_focus().map_err(|e| e.to_string())?;
        existing
            .emit("logs-visibility", true)
            .map_err(|e| e.to_string())?;
        return Ok(());
    }
    WebviewWindowBuilder::new(
        &app,
        WINDOW_LABEL,
        WebviewUrl::App("index.html?view=logs".into()),
    )
    .title("axkdeck - Logs")
    .inner_size(1100.0, 700.0)
    .min_inner_size(800.0, 600.0)
    .visible(false)
    .center()
    .build()
    .map_err(|e| e.to_string())?;
    Ok(())
}

pub fn window_event(window: &tauri::Window, event: &tauri::WindowEvent) {
    if window.label() == WINDOW_LABEL {
        if let tauri::WindowEvent::CloseRequested { api, .. } = event {
            api.prevent_close();
            let _ = window.emit("logs-visibility", false);
            if let Err(error) = window.hide() {
                log::warn!("Could not hide Logs window: {error}");
            }
        }
    } else if window.label() == "main" && matches!(event, tauri::WindowEvent::Destroyed) {
        if let Some(logs) = window.app_handle().get_webview_window(WINDOW_LABEL) {
            let _ = logs.destroy();
        }
    }
}

#[tauri::command]
pub async fn read_diagnostic_logs(
    window: WebviewWindow,
    state: State<'_, LogState>,
    request: ReadRequest,
) -> Result<Page, String> {
    require_viewer(&window)?;
    request.filter.validate()?;
    let state = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut reader = state.0.lock().map_err(|_| "Log reader is unavailable")?;
        reader.refresh()?;
        read_page(&reader, &request)
    })
    .await
    .map_err(|e| format!("Read logs: {e}"))?
}

#[tauri::command]
pub async fn clear_diagnostic_log_view(
    window: WebviewWindow,
    state: State<'_, LogState>,
) -> Result<u64, String> {
    require_viewer(&window)?;
    let state = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut reader = state.0.lock().map_err(|_| "Log reader is unavailable")?;
        reader.refresh()?;
        Ok(reader.sequence)
    })
    .await
    .map_err(|e| format!("Clear log view: {e}"))?
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SavedLog {
    path: String,
    warning: Option<String>,
}

#[tauri::command]
pub async fn save_diagnostic_logs(
    app: AppHandle,
    window: WebviewWindow,
    state: State<'_, LogState>,
    filter: Option<Filter>,
) -> Result<Option<SavedLog>, String> {
    require_viewer(&window)?;
    let filter = filter.unwrap_or_else(Filter::all);
    filter.validate()?;
    let state = state.inner().clone();
    // Freeze before showing the picker; the dialog never holds the reader lock.
    let (entries, directory) = tauri::async_runtime::spawn_blocking(move || {
        let mut reader = state.0.lock().map_err(|_| "Log reader is unavailable")?;
        reader.refresh()?;
        Ok::<_, String>((reader.matching(&filter), reader.directory().to_path_buf()))
    })
    .await
    .map_err(|e| format!("Snapshot logs: {e}"))??;
    tauri::async_runtime::spawn_blocking(move || {
        let selected = app
            .dialog()
            .file()
            .set_parent(&window)
            .set_title("Save log snapshot")
            .add_filter("Log files", &["log"])
            .set_file_name("axkdeck.log")
            .blocking_save_file();
        let Some(selected) = selected else {
            return Ok(None);
        };
        let destination = selected
            .into_path()
            .map_err(|_| "The log destination must be a local file")?;
        let destination = export::normalize_destination(destination, &directory)?;
        let warning = export::write_snapshot(&destination, &entries)?;
        Ok(Some(SavedLog {
            path: destination.to_string_lossy().into_owned(),
            warning,
        }))
    })
    .await
    .map_err(|e| format!("Save logs: {e}"))?
}

#[cfg(test)]
mod tests;
