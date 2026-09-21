use std::sync::atomic::{AtomicBool, Ordering};
use tauri::{Emitter, Manager, State};

#[derive(Default)]
pub(crate) struct EditorExitGuard(AtomicBool);

#[tauri::command]
pub(crate) fn set_editor_exit_guard(state: State<'_, EditorExitGuard>, blocked: bool) {
    state.0.store(blocked, Ordering::SeqCst);
}

#[tauri::command]
pub(crate) fn approve_editor_exit(app: tauri::AppHandle, state: State<'_, EditorExitGuard>) {
    state.0.store(false, Ordering::SeqCst);
    app.exit(0);
}

pub(crate) fn handle_exit(app: &tauri::AppHandle, event: tauri::RunEvent) {
    let tauri::RunEvent::ExitRequested { api, .. } = event else {
        return;
    };
    if app.state::<EditorExitGuard>().0.load(Ordering::SeqCst) {
        api.prevent_exit();
        if let Err(error) = app.emit_to("main", "editor-exit-requested", ()) {
            log::error!("Could not request confirmation for unsaved editor drafts: {error}");
        }
    }
}
