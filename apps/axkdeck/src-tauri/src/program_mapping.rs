use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

const WINDOW: &str = "program-mapping";

fn allows_command(window: &str, command: &str) -> bool {
    window != WINDOW
        || matches!(
            command,
            "command_program_mapping"
                | "desktop_interface_scale_mode"
                | "diagnostic_log_level"
                | "open_developer_tools"
        )
}

// Local custom commands are not scoped by plugin capabilities. Keep this
// auxiliary webview away from server credentials and direct write commands.
pub(crate) fn restricted_handler<R: tauri::Runtime>(
    handler: impl Fn(tauri::ipc::Invoke<R>) -> bool + Send + Sync + 'static,
) -> impl Fn(tauri::ipc::Invoke<R>) -> bool + Send + Sync + 'static {
    move |invoke| {
        if allows_command(
            invoke.message.webview_ref().label(),
            invoke.message.command(),
        ) {
            handler(invoke)
        } else {
            invoke
                .resolver
                .reject("Command is not available in the Mapping Editor");
            true
        }
    }
}

fn require_window(actual: &str, expected: &str) -> Result<(), String> {
    if actual == expected {
        Ok(())
    } else {
        Err("Mapping Editor command is not available in this window".to_owned())
    }
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct MappingCommand {
    request_id: String,
    context: String,
    version: u64,
    action: MappingAction,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "lowercase", deny_unknown_fields)]
enum MappingAction {
    Ready {},
    Select {
        #[serde(rename = "assignmentId")]
        assignment_id: u32,
    },
    Range {
        #[serde(rename = "assignmentId")]
        assignment_id: u32,
        range: MappingRange,
    },
    Undo {},
    Redo {},
    Save {},
    Discard {},
    Recover {},
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct MappingRange {
    low: u8,
    high: u8,
    velocity_low: u8,
    velocity_high: u8,
}

impl MappingCommand {
    fn validate(&self) -> Result<(), String> {
        if self.request_id.is_empty() || self.request_id.len() > 128 || self.context.len() > 128 {
            return Err("Invalid Mapping Editor request identity".to_owned());
        }
        if let MappingAction::Range { range, .. } = &self.action {
            if range.low > range.high
                || range.high > 127
                || range.velocity_low > range.velocity_high
                || range.velocity_high > 127
            {
                return Err("Invalid key or velocity range".to_owned());
            }
        }
        Ok(())
    }
}

#[tauri::command]
pub(crate) async fn open_program_mapping(
    app: AppHandle,
    window: WebviewWindow,
) -> Result<(), String> {
    require_window(window.label(), "main")?;
    if let Some(existing) = app.get_webview_window(WINDOW) {
        existing.show().map_err(|error| error.to_string())?;
        return existing.set_focus().map_err(|error| error.to_string());
    }
    WebviewWindowBuilder::new(
        &app,
        WINDOW,
        WebviewUrl::App("index.html?view=program-mapping".into()),
    )
    .title("Mapping Editor")
    .inner_size(1040.0, 680.0)
    .min_inner_size(800.0, 600.0)
    .visible(false)
    .build()
    .map_err(|error| error.to_string())?;
    Ok(())
}

#[tauri::command]
pub(crate) fn command_program_mapping(
    app: AppHandle,
    window: WebviewWindow,
    command: MappingCommand,
) -> Result<(), String> {
    require_window(window.label(), WINDOW)?;
    command.validate()?;
    let main = app
        .get_webview_window("main")
        .ok_or_else(|| "The main Program editor is closed".to_owned())?;
    main.emit("program-mapping-command", &command)
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub(crate) fn publish_program_mapping(
    app: AppHandle,
    window: WebviewWindow,
    message: serde_json::Value,
) -> Result<(), String> {
    require_window(window.label(), "main")?;
    let encoded = serde_json::to_vec(&message).map_err(|error| error.to_string())?;
    if encoded.len() > 16 * 1024 * 1024 {
        return Err("Mapping Editor snapshot is too large".to_owned());
    }
    if let Some(editor) = app.get_webview_window(WINDOW) {
        editor
            .emit("program-mapping-state", &message)
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn mapping_window_cannot_obtain_credentials_or_invoke_write_commands() {
        for command in [
            "server_connection",
            "use_local_server",
            "configure_remote_server",
            "save_retained_package",
            "save_allocation_map_json",
            "publish_program_mapping",
        ] {
            assert!(!super::allows_command(super::WINDOW, command));
            assert!(super::allows_command("main", command));
        }
        assert!(super::allows_command(
            super::WINDOW,
            "command_program_mapping"
        ));
        assert!(super::allows_command(
            super::WINDOW,
            "desktop_interface_scale_mode"
        ));
    }
    use super::{MappingCommand, require_window};

    #[test]
    fn relay_only_accepts_its_own_window_and_known_commands() {
        assert!(require_window("main", "main").is_ok());
        assert!(require_window("program-mapping", "main").is_err());
        assert!(require_window("logs", "program-mapping").is_err());
        for action in [
            r#"{"kind":"write"}"#,
            r#"{"kind":"save","path":"image.hds"}"#,
        ] {
            let value =
                format!(r#"{{"requestId":"one","context":"ctx","version":1,"action":{action}}}"#);
            assert!(serde_json::from_str::<MappingCommand>(&value).is_err());
        }
        for kind in ["ready", "undo", "redo", "save", "discard", "recover"] {
            let valid = format!(
                r#"{{"requestId":"one","context":"ctx","version":1,"action":{{"kind":"{kind}"}}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&valid).is_ok());
            let invalid = format!(
                r#"{{"requestId":"one","context":"ctx","version":1,"action":{{"kind":"{kind}","path":"image.hds"}}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&invalid).is_err());
        }
    }

    #[test]
    fn range_relay_enforces_both_midi_domains_and_order() {
        for (low, high, velocity_high, expected) in [
            (0, 127, 127, true),
            (80, 40, 127, false),
            (0, 128, 127, false),
            (0, 127, 128, false),
        ] {
            let value = format!(
                r#"{{"requestId":"one","context":"ctx","version":1,"action":{{"kind":"range","assignmentId":0,"range":{{"low":{low},"high":{high},"velocityLow":0,"velocityHigh":{velocity_high}}}}}}}"#
            );
            let command = serde_json::from_str::<MappingCommand>(&value).expect("decode command");
            assert_eq!(command.validate().is_ok(), expected);
        }
    }
}
