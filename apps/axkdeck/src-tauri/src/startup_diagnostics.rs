use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use tauri::State;

const FRONTEND_SCHEMA_VERSION: u32 = 2;
const MAXIMUM_FRONTEND_MILLISECONDS: f64 = 60.0 * 60.0 * 1000.0;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StartupMilestone {
    NativeEntry,
    PlatformConfigurationCompleted,
    LogPluginBuildStarted,
    LogPluginBuildCompleted,
    TauriBuilderConfigured,
    TauriSetupStarted,
    PathsResolved,
    PreferencesLoadStarted,
    PreferencesLoadCompleted,
    CredentialLookupStarted,
    CredentialLookupCompleted,
    SidecarStartupStarted,
    SidecarStatePrepared,
    SidecarSpawned,
    SidecarReadinessReceived,
    SidecarConnectionValidated,
    SidecarStartupCompleted,
    TauriSetupCompleted,
    PageLoadStarted,
    PageLoadFinished,
}

impl StartupMilestone {
    fn name(self) -> &'static str {
        match self {
            Self::NativeEntry => "native_entry",
            Self::PlatformConfigurationCompleted => "platform_configuration_completed",
            Self::LogPluginBuildStarted => "log_plugin_build_started",
            Self::LogPluginBuildCompleted => "log_plugin_build_completed",
            Self::TauriBuilderConfigured => "tauri_builder_configured",
            Self::TauriSetupStarted => "tauri_setup_started",
            Self::PathsResolved => "paths_resolved",
            Self::PreferencesLoadStarted => "preferences_load_started",
            Self::PreferencesLoadCompleted => "preferences_load_completed",
            Self::CredentialLookupStarted => "credential_lookup_started",
            Self::CredentialLookupCompleted => "credential_lookup_completed",
            Self::SidecarStartupStarted => "sidecar_startup_started",
            Self::SidecarStatePrepared => "sidecar_state_prepared",
            Self::SidecarSpawned => "sidecar_spawned",
            Self::SidecarReadinessReceived => "sidecar_readiness_received",
            Self::SidecarConnectionValidated => "sidecar_connection_validated",
            Self::SidecarStartupCompleted => "sidecar_startup_completed",
            Self::TauriSetupCompleted => "tauri_setup_completed",
            Self::PageLoadStarted => "page_load_started",
            Self::PageLoadFinished => "page_load_finished",
        }
    }
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CredentialOutcome {
    Found,
    Missing,
    Error,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ServerOutcome {
    RemoteReady,
    LocalReady,
    Disabled,
    BinaryUnavailable,
    Failed,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FrontendStartupMetrics {
    schema_version: u32,
    view: StartupView,
    module_evaluated_ms: f64,
    diagnostics_installed_ms: f64,
    interface_scale_complete_ms: f64,
    shell_mounted_ms: f64,
    shell_first_frame_ms: f64,
    app_module_ready_ms: f64,
    server_connection_complete_ms: Option<f64>,
    app_mounted_ms: f64,
    app_first_frame_ms: f64,
    navigation_duration_ms: Option<f64>,
    first_contentful_paint_ms: Option<f64>,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum StartupView {
    Workspace,
    Allocation,
}

#[derive(Clone, Copy, Debug)]
struct MilestoneEntry {
    milestone: StartupMilestone,
    elapsed: Duration,
}

#[derive(Default)]
struct StartupState {
    milestones: Vec<MilestoneEntry>,
    credential_outcome: Option<CredentialOutcome>,
    server_outcome: Option<ServerOutcome>,
    logging_enabled: bool,
    completed: bool,
}

#[derive(Clone)]
pub struct StartupDiagnostics {
    origin: Instant,
    state: Arc<Mutex<StartupState>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct StartupMilestoneEvent {
    event: &'static str,
    schema_version: u32,
    milestone: &'static str,
    elapsed_ms: u128,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct StartupSummary {
    event: &'static str,
    schema_version: u32,
    total_duration_ms: u128,
    platform_configuration_completed_ms: Option<u128>,
    log_plugin_build_ms: Option<u128>,
    tauri_builder_configured_ms: Option<u128>,
    setup_duration_ms: Option<u128>,
    preferences_load_ms: Option<u128>,
    credential_lookup_ms: Option<u128>,
    sidecar_startup_ms: Option<u128>,
    page_load_started_ms: Option<u128>,
    page_load_finished_ms: Option<u128>,
    credential_outcome: Option<CredentialOutcome>,
    server_outcome: Option<ServerOutcome>,
    platform: &'static str,
    architecture: &'static str,
    build_profile: &'static str,
    source_identity: &'static str,
    frontend: FrontendStartupSummary,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct FrontendStartupSummary {
    view: StartupView,
    module_evaluated_ms: f64,
    diagnostics_installed_ms: f64,
    interface_scale_complete_ms: f64,
    shell_mounted_ms: f64,
    shell_first_frame_ms: f64,
    app_module_ready_ms: f64,
    server_connection_complete_ms: Option<f64>,
    app_mounted_ms: f64,
    app_first_frame_ms: f64,
    navigation_duration_ms: Option<f64>,
    first_contentful_paint_ms: Option<f64>,
}

impl StartupDiagnostics {
    pub fn new() -> Self {
        Self {
            origin: Instant::now(),
            state: Arc::new(Mutex::new(StartupState::default())),
        }
    }

    pub fn record(&self, milestone: StartupMilestone) {
        self.record_at(milestone, self.origin.elapsed());
    }

    fn record_at(&self, milestone: StartupMilestone, elapsed: Duration) {
        let should_log = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            if state
                .milestones
                .iter()
                .any(|entry| entry.milestone == milestone)
            {
                return;
            }
            state.milestones.push(MilestoneEntry { milestone, elapsed });
            state.logging_enabled
        };
        if should_log {
            log_milestone(milestone, elapsed);
        }
    }

    pub fn enable_logging(&self) {
        let entries = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            if state.logging_enabled {
                return;
            }
            state.logging_enabled = true;
            state.milestones.clone()
        };
        for entry in entries {
            log_milestone(entry.milestone, entry.elapsed);
        }
    }

    pub fn set_credential_outcome(&self, outcome: CredentialOutcome) {
        if let Ok(mut state) = self.state.lock() {
            state.credential_outcome = Some(outcome);
        }
    }

    pub fn set_server_outcome(&self, outcome: ServerOutcome) {
        if let Ok(mut state) = self.state.lock() {
            state.server_outcome = Some(outcome);
        }
    }

    fn complete(&self, metrics: FrontendStartupMetrics) -> Result<(), String> {
        validate_frontend_metrics(&metrics)?;
        let summary = self.summary(metrics, self.origin.elapsed())?;
        let encoded = serde_json::to_string(&summary)
            .map_err(|error| format!("encode desktop startup summary: {error}"))?;
        log::info!("{encoded}");
        Ok(())
    }

    fn summary(
        &self,
        metrics: FrontendStartupMetrics,
        elapsed: Duration,
    ) -> Result<StartupSummary, String> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| "desktop startup diagnostics are unavailable".to_owned())?;
        if state.completed {
            return Err("desktop startup diagnostics were already completed".to_owned());
        }
        state.completed = true;
        let duration = |start, end| milestone_duration(&state.milestones, start, end);
        let milestone = |target| milestone_elapsed(&state.milestones, target);
        Ok(StartupSummary {
            event: "desktop_startup_completed",
            schema_version: FRONTEND_SCHEMA_VERSION,
            total_duration_ms: elapsed.as_millis(),
            platform_configuration_completed_ms: milestone(
                StartupMilestone::PlatformConfigurationCompleted,
            ),
            log_plugin_build_ms: duration(
                StartupMilestone::LogPluginBuildStarted,
                StartupMilestone::LogPluginBuildCompleted,
            ),
            tauri_builder_configured_ms: milestone(StartupMilestone::TauriBuilderConfigured),
            setup_duration_ms: duration(
                StartupMilestone::TauriSetupStarted,
                StartupMilestone::TauriSetupCompleted,
            ),
            preferences_load_ms: duration(
                StartupMilestone::PreferencesLoadStarted,
                StartupMilestone::PreferencesLoadCompleted,
            ),
            credential_lookup_ms: duration(
                StartupMilestone::CredentialLookupStarted,
                StartupMilestone::CredentialLookupCompleted,
            ),
            sidecar_startup_ms: duration(
                StartupMilestone::SidecarStartupStarted,
                StartupMilestone::SidecarStartupCompleted,
            ),
            page_load_started_ms: milestone(StartupMilestone::PageLoadStarted),
            page_load_finished_ms: milestone(StartupMilestone::PageLoadFinished),
            credential_outcome: state.credential_outcome,
            server_outcome: state.server_outcome,
            platform: std::env::consts::OS,
            architecture: std::env::consts::ARCH,
            build_profile: if cfg!(debug_assertions) {
                "debug"
            } else {
                "release"
            },
            source_identity: env!("AXKDECK_SOURCE_IDENTITY"),
            frontend: FrontendStartupSummary {
                view: metrics.view,
                module_evaluated_ms: metrics.module_evaluated_ms,
                diagnostics_installed_ms: metrics.diagnostics_installed_ms,
                interface_scale_complete_ms: metrics.interface_scale_complete_ms,
                shell_mounted_ms: metrics.shell_mounted_ms,
                shell_first_frame_ms: metrics.shell_first_frame_ms,
                app_module_ready_ms: metrics.app_module_ready_ms,
                server_connection_complete_ms: metrics.server_connection_complete_ms,
                app_mounted_ms: metrics.app_mounted_ms,
                app_first_frame_ms: metrics.app_first_frame_ms,
                navigation_duration_ms: metrics.navigation_duration_ms,
                first_contentful_paint_ms: metrics.first_contentful_paint_ms,
            },
        })
    }
}

fn milestone_elapsed(entries: &[MilestoneEntry], target: StartupMilestone) -> Option<u128> {
    entries
        .iter()
        .find(|entry| entry.milestone == target)
        .map(|entry| entry.elapsed.as_millis())
}

fn milestone_duration(
    entries: &[MilestoneEntry],
    start: StartupMilestone,
    end: StartupMilestone,
) -> Option<u128> {
    let start = entries
        .iter()
        .find(|entry| entry.milestone == start)?
        .elapsed;
    let end = entries.iter().find(|entry| entry.milestone == end)?.elapsed;
    end.checked_sub(start).map(|duration| duration.as_millis())
}

fn validate_frontend_metrics(metrics: &FrontendStartupMetrics) -> Result<(), String> {
    if metrics.schema_version != FRONTEND_SCHEMA_VERSION {
        return Err("unsupported desktop startup metrics schema".to_owned());
    }
    let ordered = [
        metrics.module_evaluated_ms,
        metrics.shell_mounted_ms,
        metrics.shell_first_frame_ms,
        metrics.app_module_ready_ms,
        metrics.app_mounted_ms,
        metrics.app_first_frame_ms,
    ];
    if ordered
        .iter()
        .any(|value| !value.is_finite() || *value < 0.0 || *value > MAXIMUM_FRONTEND_MILLISECONDS)
        || ordered.windows(2).any(|pair| pair[0] > pair[1])
    {
        return Err("desktop startup metrics are invalid or unordered".to_owned());
    }
    for setup in [
        metrics.diagnostics_installed_ms,
        metrics.interface_scale_complete_ms,
    ] {
        if !setup.is_finite()
            || !(metrics.module_evaluated_ms..=metrics.app_mounted_ms).contains(&setup)
        {
            return Err("desktop startup setup metric is invalid".to_owned());
        }
    }
    if let Some(server) = metrics.server_connection_complete_ms {
        if !server.is_finite()
            || !(metrics.module_evaluated_ms..=metrics.app_mounted_ms).contains(&server)
        {
            return Err("desktop startup server metric is invalid".to_owned());
        }
    }
    for optional in [
        metrics.navigation_duration_ms,
        metrics.first_contentful_paint_ms,
    ]
    .into_iter()
    .flatten()
    {
        if !optional.is_finite() || !(0.0..=MAXIMUM_FRONTEND_MILLISECONDS).contains(&optional) {
            return Err("desktop startup browser metrics are invalid".to_owned());
        }
    }
    Ok(())
}

fn log_milestone(milestone: StartupMilestone, elapsed: Duration) {
    let event = StartupMilestoneEvent {
        event: "desktop_startup_milestone",
        schema_version: FRONTEND_SCHEMA_VERSION,
        milestone: milestone.name(),
        elapsed_ms: elapsed.as_millis(),
    };
    if let Ok(encoded) = serde_json::to_string(&event) {
        log::debug!("{encoded}");
    }
}

#[tauri::command]
pub fn complete_startup(
    metrics: FrontendStartupMetrics,
    diagnostics: State<'_, StartupDiagnostics>,
) -> Result<(), String> {
    diagnostics.complete(metrics)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_metrics() -> FrontendStartupMetrics {
        FrontendStartupMetrics {
            schema_version: FRONTEND_SCHEMA_VERSION,
            view: StartupView::Workspace,
            module_evaluated_ms: 2.0,
            diagnostics_installed_ms: 4.0,
            interface_scale_complete_ms: 6.0,
            shell_mounted_ms: 3.0,
            shell_first_frame_ms: 8.0,
            app_module_ready_ms: 9.0,
            server_connection_complete_ms: Some(8.5),
            app_mounted_ms: 10.0,
            app_first_frame_ms: 12.0,
            navigation_duration_ms: Some(10.0),
            first_contentful_paint_ms: Some(11.0),
        }
    }

    #[test]
    fn validates_ordered_bounded_frontend_metrics() {
        assert!(validate_frontend_metrics(&valid_metrics()).is_ok());
        let mut unordered = valid_metrics();
        unordered.app_mounted_ms = 3.0;
        assert!(validate_frontend_metrics(&unordered).is_err());
        let mut unsupported = valid_metrics();
        unsupported.schema_version = 1;
        assert!(validate_frontend_metrics(&unsupported).is_err());
    }

    #[test]
    fn summarizes_each_startup_once_without_sensitive_values() {
        let diagnostics = StartupDiagnostics::new();
        diagnostics.record_at(
            StartupMilestone::TauriSetupStarted,
            Duration::from_millis(2),
        );
        diagnostics.record_at(
            StartupMilestone::TauriSetupCompleted,
            Duration::from_millis(7),
        );
        diagnostics.set_credential_outcome(CredentialOutcome::Missing);
        diagnostics.set_server_outcome(ServerOutcome::LocalReady);
        let summary = diagnostics
            .summary(valid_metrics(), Duration::from_millis(20))
            .expect("summarize startup");
        let encoded = serde_json::to_string(&summary).expect("encode startup summary");
        assert_eq!(summary.setup_duration_ms, Some(5));
        assert!(encoded.contains("desktop_startup_completed"));
        for forbidden in ["bearerToken", "baseUrl", "logDirectory", "stateDirectory"] {
            assert!(!encoded.contains(forbidden));
        }
        assert!(
            diagnostics
                .summary(valid_metrics(), Duration::from_millis(21))
                .is_err()
        );
    }

    #[test]
    fn records_each_fixed_milestone_only_once() {
        let diagnostics = StartupDiagnostics::new();
        diagnostics.record_at(StartupMilestone::NativeEntry, Duration::from_millis(1));
        diagnostics.record_at(StartupMilestone::NativeEntry, Duration::from_millis(9));
        let state = diagnostics.state.lock().expect("startup state");
        assert_eq!(state.milestones.len(), 1);
        assert_eq!(state.milestones[0].elapsed, Duration::from_millis(1));
    }
}
