use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use tauri::{AppHandle, Manager, State, WebviewWindow};

use crate::local_directory_exports::download_retained_directory_export;
use crate::native_drag_cache::{DragCache, Limits};
use crate::remote_settings::ServerConnectionState;
use crate::server_sidecar::FrontendConnection;

#[derive(Default)]
pub(crate) struct DragState {
    cache: Option<DragCache>,
    connections: HashMap<String, FrontendConnection>,
    active: bool,
}
pub(crate) type NativeDragState = Arc<Mutex<DragState>>;

impl DragState {
    fn prune(&mut self) -> Result<(), String> {
        if let Some(cache) = &mut self.cache {
            cache.prune(now())?;
            self.connections.retain(|id, _| cache.reserved(id));
        }
        Ok(())
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
fn main_window(window: &WebviewWindow) -> Result<(), String> {
    if window.label() == "main" {
        Ok(())
    } else {
        Err("Native drag export is restricted to the main window".to_owned())
    }
}
fn unavailable() -> String {
    "Native drag export state is unavailable".to_owned()
}

pub(crate) fn state() -> NativeDragState {
    let state = Arc::new(Mutex::new(DragState::default()));
    let weak = Arc::downgrade(&state);
    std::thread::spawn(move || {
        loop {
            std::thread::sleep(Duration::from_secs(60));
            let Some(state) = weak.upgrade() else {
                break;
            };
            let Ok(mut state) = state.lock() else {
                break;
            };
            if let Err(error) = state.prune() {
                log::warn!("{error}");
            }
        }
    });
    state
}

#[tauri::command]
pub(crate) async fn reserve_native_files_drag(
    app: AppHandle,
    window: WebviewWindow,
    expected_size: u64,
    state: State<'_, NativeDragState>,
    connections: State<'_, ServerConnectionState>,
) -> Result<String, String> {
    main_window(&window)?;
    let state = state.inner().clone();
    let connections = connections.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let connection = connections
            .connection()?
            .ok_or_else(|| "axklib-server is unavailable".to_owned())?;
        let mut state = state.lock().map_err(|_| unavailable())?;
        if state.active {
            return Err("A native file drag is already active".to_owned());
        }
        if state.cache.is_none() {
            let parent = app
                .path()
                .app_cache_dir()
                .map_err(|error| error.to_string())?;
            std::fs::create_dir_all(&parent).map_err(|error| error.to_string())?;
            state.cache = Some(DragCache::open(
                &parent.join("drag-exports"),
                Limits::default(),
            )?);
        }
        state.prune()?;
        let preparation = state
            .cache
            .as_mut()
            .unwrap()
            .reserve(expected_size, now())?;
        state.connections.insert(preparation.id.clone(), connection);
        Ok(preparation.id)
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
pub(crate) async fn prepare_native_files_drag(
    window: WebviewWindow,
    ticket: String,
    content_path: String,
    state: State<'_, NativeDragState>,
) -> Result<(), String> {
    main_window(&window)?;
    let state = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (preparation, connection) = {
            let mut state = state.lock().map_err(|_| unavailable())?;
            let connection = state
                .connections
                .remove(&ticket)
                .ok_or_else(|| "Native export ticket expired".to_owned())?;
            let preparation = state
                .cache
                .as_mut()
                .ok_or_else(unavailable)?
                .begin(&ticket)?;
            (preparation, connection)
        };
        let result = download_retained_directory_export(
            connection,
            preparation.destination,
            content_path,
            preparation.control.maximum_archive_bytes,
            &preparation.control,
        )
        .and_then(|_| preparation.control.check());
        state
            .lock()
            .map_err(|_| unavailable())?
            .cache
            .as_mut()
            .ok_or_else(unavailable)?
            .finish(&ticket, result.is_ok(), now())?;
        result
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
pub(crate) async fn cancel_native_files_drag(
    window: WebviewWindow,
    ticket: String,
    state: State<'_, NativeDragState>,
) -> Result<(), String> {
    main_window(&window)?;
    let state = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut state = state.lock().map_err(|_| unavailable())?;
        state.connections.remove(&ticket);
        if let Some(cache) = &mut state.cache {
            cache.cancel(&ticket)?;
        }
        Ok(())
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
pub(crate) async fn start_native_files_drag(
    window: WebviewWindow,
    ticket: String,
    state: State<'_, NativeDragState>,
) -> Result<(), String> {
    main_window(&window)?;
    let state = state.inner().clone();
    let paths = {
        let mut state = state.lock().map_err(|_| unavailable())?;
        if state.active {
            return Err("A native file drag is already active".to_owned());
        }
        let paths = state
            .cache
            .as_mut()
            .ok_or_else(unavailable)?
            .claim(&ticket, now())?;
        state.active = true;
        paths
    };
    let (finished_sender, finished_receiver) = std::sync::mpsc::channel();
    let finish = Arc::new(move |dropped: bool| {
        if let Ok(mut state) = state.lock() {
            state.active = false;
            if let Some(cache) = &mut state.cache {
                if let Err(error) = cache.end(&ticket, dropped) {
                    log::warn!("{error}");
                }
            }
        }
        let _ = finished_sender.send(dropped);
    });
    let callback = finish.clone();
    let target = window.clone();
    let (sender, receiver) = std::sync::mpsc::channel();
    if let Err(error) = window.run_on_main_thread(move || {
        let result = crate::native_drag_platform::start(&target, paths, callback.clone());
        if result.is_err() {
            callback(false);
        }
        let _ = sender.send(result);
    }) {
        finish(false);
        return Err(error.to_string());
    }
    tauri::async_runtime::spawn_blocking(move || {
        receiver.recv().map_err(|error| error.to_string())??;
        finished_receiver
            .recv()
            .map_err(|error| error.to_string())?;
        Ok(())
    })
    .await
    .map_err(|error| error.to_string())?
}
