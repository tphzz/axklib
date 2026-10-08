use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

const WINDOWS: [&str; 4] = [
    "mapping-program",
    "mapping-sample",
    "mapping-bank",
    "mapping-members",
];

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum MappingRole {
    Program,
    Sample,
    Bank,
    Members,
}
impl MappingRole {
    fn name(self) -> &'static str {
        match self {
            Self::Program => "program",
            Self::Sample => "sample",
            Self::Bank => "bank",
            Self::Members => "members",
        }
    }
    fn window(self) -> &'static str {
        match self {
            Self::Program => WINDOWS[0],
            Self::Sample => WINDOWS[1],
            Self::Bank => WINDOWS[2],
            Self::Members => WINDOWS[3],
        }
    }
}

fn allows_command(window: &str, command: &str) -> bool {
    !WINDOWS.contains(&window)
        || matches!(
            command,
            "command_mapping_editor"
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
    role: MappingRole,
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
        #[serde(rename = "selectionId")]
        selection_id: u32,
    },
    Range {
        #[serde(rename = "selectionId")]
        selection_id: u32,
        range: MappingRange,
        boundaries: Vec<RangeBoundary>,
    },
    Move {
        #[serde(rename = "selectionId")]
        selection_id: u32,
        #[serde(rename = "editRevision")]
        edit_revision: u64,
        original: MappingRange,
        range: MappingRange,
        boundaries: Vec<RangeBoundary>,
    },
    Inherit {
        boundary: RangeBoundary,
    },
    Root {
        #[serde(rename = "selectionId")]
        selection_id: u32,
        note: u8,
    },
    Note {
        #[serde(rename = "clientId")]
        client_id: String,
        #[serde(rename = "imageRevision")]
        image_revision: u64,
        sequence: u64,
        note: u8,
        velocity: u8,
    },
    Release {
        #[serde(rename = "clientId")]
        client_id: String,
        sequence: u64,
    },
    Lease {
        #[serde(rename = "clientId")]
        client_id: String,
        sequence: u64,
    },
    Undo {},
    Redo {},
    Save {},
    Discard {},
    Recover {},
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
enum RangeBoundary {
    Low,
    High,
    VelocityLow,
    VelocityHigh,
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
        if let MappingAction::Range {
            range, boundaries, ..
        }
        | MappingAction::Move {
            range, boundaries, ..
        } = &self.action
        {
            if range.low > range.high
                || range.high > 127
                || range.velocity_low > range.velocity_high
                || range.velocity_high > 127
                || boundaries.is_empty()
                || boundaries.len() > 4
                || boundaries
                    .iter()
                    .enumerate()
                    .any(|(index, value)| boundaries[..index].contains(value))
            {
                return Err("Invalid key or velocity range".to_owned());
            }
        }
        if let MappingAction::Move {
            original, range, ..
        } = &self.action
        {
            if original.low > original.high
                || original.high > 127
                || original.velocity_low > original.velocity_high
                || original.velocity_high > 127
                || range.high - range.low != original.high - original.low
                || range.velocity_high - range.velocity_low
                    != original.velocity_high - original.velocity_low
            {
                return Err("Moving a mapping must preserve its dimensions".to_owned());
            }
        }
        if let MappingAction::Root { note, .. } = self.action {
            if !matches!(self.role, MappingRole::Sample) || note > 127 {
                return Err("Root editing is not available in this mapping context".to_owned());
            }
        }
        if let MappingAction::Note {
            client_id,
            sequence,
            ..
        }
        | MappingAction::Release {
            client_id,
            sequence,
        }
        | MappingAction::Lease {
            client_id,
            sequence,
        } = &self.action
        {
            if client_id.is_empty()
                || client_id.len() > 128
                || *sequence == 0
                || *sequence > 9_007_199_254_740_991
            {
                return Err("Invalid held-note identity".to_owned());
            }
        }
        if let MappingAction::Note {
            note,
            velocity,
            image_revision,
            ..
        } = self.action
        {
            if note > 127
                || velocity == 0
                || velocity > 127
                || image_revision > 9_007_199_254_740_991
            {
                return Err("Invalid audition note or velocity".to_owned());
            }
        }
        Ok(())
    }
}

#[tauri::command]
pub(crate) async fn open_mapping_editor(
    app: AppHandle,
    window: WebviewWindow,
    role: MappingRole,
) -> Result<(), String> {
    require_window(window.label(), "main")?;
    if let Some(existing) = app.get_webview_window(role.window()) {
        existing.show().map_err(|error| error.to_string())?;
        return existing.set_focus().map_err(|error| error.to_string());
    }
    WebviewWindowBuilder::new(
        &app,
        role.window(),
        WebviewUrl::App(format!("index.html?view=mapping-editor&role={}", role.name()).into()),
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
pub(crate) fn command_mapping_editor(
    app: AppHandle,
    window: WebviewWindow,
    command: MappingCommand,
) -> Result<(), String> {
    require_window(window.label(), command.role.window())?;
    command.validate()?;
    let main = app
        .get_webview_window("main")
        .ok_or_else(|| "The main editor is closed".to_owned())?;
    main.emit("mapping-editor-command", &command)
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub(crate) fn publish_mapping_editor(
    app: AppHandle,
    window: WebviewWindow,
    role: MappingRole,
    message: serde_json::Value,
) -> Result<(), String> {
    require_window(window.label(), "main")?;
    let encoded = serde_json::to_vec(&message).map_err(|error| error.to_string())?;
    if encoded.len() > 16 * 1024 * 1024 {
        return Err("Mapping Editor snapshot is too large".to_owned());
    }
    if message
        .pointer("/state/role")
        .and_then(serde_json::Value::as_str)
        != Some(role.name())
    {
        return Err("Mapping Editor snapshot has the wrong context".to_owned());
    }
    if let Some(editor) = app.get_webview_window(role.window()) {
        editor
            .emit("mapping-editor-state", &message)
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn held_note_relay_bounds_identity_note_and_velocity_and_rejects_extra_fields() {
        for (note, velocity, expected) in [
            (0, 1, true),
            (127, 127, true),
            (128, 100, false),
            (60, 0, false),
            (60, 128, false),
        ] {
            let value = format!(
                r#"{{"role":"program","requestId":"one","context":"ctx","version":1,"action":{{"kind":"note","clientId":"child","sequence":1,"note":{note},"velocity":{velocity},"imageRevision":1}}}}"#
            );
            assert_eq!(
                serde_json::from_str::<MappingCommand>(&value)
                    .expect("decode note")
                    .validate()
                    .is_ok(),
                expected
            );
        }
        for kind in ["release", "lease"] {
            let value = format!(
                r#"{{"role":"sample","requestId":"one","context":"ctx","version":1,"action":{{"kind":"{kind}","clientId":"child","sequence":1}}}}"#
            );
            assert!(
                serde_json::from_str::<MappingCommand>(&value)
                    .expect("decode held control")
                    .validate()
                    .is_ok()
            );
        }
        for action in [
            r#"{"kind":"note","clientId":"child","sequence":1,"note":60.5,"velocity":100,"imageRevision":1}"#,
            r#"{"kind":"note","clientId":"child","sequence":1,"note":60,"velocity":100}"#,
            r#"{"kind":"note","clientId":"child","sequence":1,"note":60,"velocity":100,"imageRevision":-1}"#,
            r#"{"kind":"note","clientId":"child","sequence":1,"note":60,"velocity":100,"imageRevision":9007199254740992}"#,
            r#"{"kind":"release","clientId":"child","sequence":1,"path":"image.hds"}"#,
            r#"{"kind":"lease","clientId":"child","sequence":0}"#,
        ] {
            let value = format!(
                r#"{{"role":"sample","requestId":"one","context":"ctx","version":1,"action":{action}}}"#
            );
            assert!(
                serde_json::from_str::<MappingCommand>(&value)
                    .map_or(true, |command| command.validate().is_err())
            );
        }
    }
    #[test]
    fn move_relay_requires_an_unchanged_rectangle_size() {
        for (high, expected) in [(44, true), (45, false)] {
            let value = format!(
                r#"{{"role":"members","requestId":"one","context":"ctx","version":1,"action":{{"kind":"move","selectionId":0,"editRevision":2,"original":{{"low":20,"high":40,"velocityLow":30,"velocityHigh":60}},"range":{{"low":24,"high":{high},"velocityLow":32,"velocityHigh":62}},"boundaries":["low","high","velocityLow","velocityHigh"]}}}}"#
            );
            assert_eq!(
                serde_json::from_str::<MappingCommand>(&value)
                    .expect("decode move")
                    .validate()
                    .is_ok(),
                expected
            );
        }
    }
    #[test]
    fn mapping_window_cannot_obtain_credentials_or_invoke_write_commands() {
        for command in [
            "server_connection",
            "use_local_server",
            "configure_remote_server",
            "save_retained_package",
            "save_allocation_map_json",
            "publish_mapping_editor",
        ] {
            for window in super::WINDOWS {
                assert!(!super::allows_command(window, command));
            }
            assert!(super::allows_command("main", command));
        }
        assert!(super::allows_command(
            super::WINDOWS[0],
            "command_mapping_editor"
        ));
        assert!(super::allows_command(
            super::WINDOWS[0],
            "desktop_interface_scale_mode"
        ));
    }
    use super::{MappingCommand, require_window};

    #[test]
    fn root_relay_is_sample_only_and_strictly_typed() {
        for role in ["program", "sample", "bank", "members"] {
            for note in [0, 127, 128, 255] {
                let value = format!(
                    r#"{{"role":"{role}","requestId":"one","context":"ctx","version":1,"action":{{"kind":"root","selectionId":0,"note":{note}}}}}"#
                );
                let command = serde_json::from_str::<MappingCommand>(&value).expect("decode root");
                assert_eq!(command.validate().is_ok(), role == "sample" && note <= 127);
            }
        }
        for action in [
            r#"{"kind":"root","selectionId":0}"#,
            r#"{"kind":"root","selectionId":0,"note":60,"path":"image.hds"}"#,
            r#"{"kind":"root","selectionId":0,"note":-1}"#,
            r#"{"kind":"root","selectionId":0,"note":60.5}"#,
            r#"{"kind":"root","selectionId":0,"note":"60"}"#,
            r#"{"kind":"root","selectionId":0,"note":null}"#,
            r#"{"kind":"root","selectionId":0,"note":true}"#,
        ] {
            let value = format!(
                r#"{{"role":"sample","requestId":"one","context":"ctx","version":1,"action":{action}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&value).is_err());
        }
    }

    #[test]
    fn relay_only_accepts_its_own_window_and_known_commands() {
        assert!(require_window("main", "main").is_ok());
        assert!(require_window("mapping-program", "main").is_err());
        assert!(require_window("logs", "mapping-program").is_err());
        assert!(require_window("mapping-members", "mapping-bank").is_err());
        for action in [
            r#"{"kind":"write"}"#,
            r#"{"kind":"save","path":"image.hds"}"#,
        ] {
            let value = format!(
                r#"{{"role":"program","requestId":"one","context":"ctx","version":1,"action":{action}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&value).is_err());
        }
        for kind in ["ready", "undo", "redo", "save", "discard", "recover"] {
            let valid = format!(
                r#"{{"role":"program","requestId":"one","context":"ctx","version":1,"action":{{"kind":"{kind}"}}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&valid).is_ok());
            let invalid = format!(
                r#"{{"role":"program","requestId":"one","context":"ctx","version":1,"action":{{"kind":"{kind}","path":"image.hds"}}}}"#
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
                r#"{{"role":"program","requestId":"one","context":"ctx","version":1,"action":{{"kind":"range","selectionId":0,"boundaries":["low","high","velocityLow","velocityHigh"],"range":{{"low":{low},"high":{high},"velocityLow":0,"velocityHigh":{velocity_high}}}}}}}"#
            );
            let command = serde_json::from_str::<MappingCommand>(&value).expect("decode command");
            assert_eq!(command.validate().is_ok(), expected);
        }
    }

    #[test]
    fn relay_rejects_unknown_roles_obsolete_fields_and_invalid_boundary_sets() {
        for action in [
            r#"{"kind":"select","assignmentId":0}"#,
            r#"{"kind":"inherit","boundary":"root"}"#,
        ] {
            let value = format!(
                r#"{{"role":"members","requestId":"one","context":"ctx","version":1,"action":{action}}}"#
            );
            assert!(serde_json::from_str::<MappingCommand>(&value).is_err());
        }
        let unknown = r#"{"role":"other","requestId":"one","context":"ctx","version":1,"action":{"kind":"ready"}}"#;
        assert!(serde_json::from_str::<MappingCommand>(unknown).is_err());
        for boundaries in ["[]", r#"["low","low"]"#] {
            let value = format!(
                r#"{{"role":"sample","requestId":"one","context":"ctx","version":1,"action":{{"kind":"range","selectionId":0,"range":{{"low":0,"high":127,"velocityLow":0,"velocityHigh":127}},"boundaries":{boundaries}}}}}"#
            );
            assert!(
                serde_json::from_str::<MappingCommand>(&value)
                    .expect("decode")
                    .validate()
                    .is_err()
            );
        }
    }
}
