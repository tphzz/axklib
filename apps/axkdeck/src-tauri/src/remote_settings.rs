use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex};
use url::{Host, Url};

use crate::server_sidecar::{FrontendConnection, ServerSidecar};
use crate::startup_diagnostics::{
    CredentialOutcome, ServerOutcome, StartupDiagnostics, StartupMilestone,
};

const KEYRING_SERVICE: &str = "app.axkdeck.desktop.axklib-server";
const KEYRING_USER: &str = "remote-server-v1";
const MINIMUM_TOKEN_BYTES: usize = 32;
const MAXIMUM_TOKEN_BYTES: usize = 4096;

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoteServerSettingsInput {
    pub base_url: String,
    pub bearer_token: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct StoredRemoteSettings {
    base_url: String,
    bearer_token: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoteServerSettingsView {
    pub mode: String,
    pub base_url: Option<String>,
    pub token_configured: bool,
    pub secure_storage_error: Option<String>,
}

pub struct ServerConnectionManager {
    sidecar: Option<ServerSidecar>,
    connection: Option<FrontendConnection>,
    secure_storage_error: Option<String>,
    log_directory: PathBuf,
    state_directory: PathBuf,
    workspace_store: PathBuf,
}

struct ServerConnectionStateInner {
    manager: Mutex<Option<ServerConnectionManager>>,
    ready: Condvar,
}

#[derive(Clone)]
pub struct ServerConnectionState {
    inner: Arc<ServerConnectionStateInner>,
}

impl ServerConnectionState {
    pub fn pending() -> Self {
        Self {
            inner: Arc::new(ServerConnectionStateInner {
                manager: Mutex::new(None),
                ready: Condvar::new(),
            }),
        }
    }

    pub fn initialize_in_background(
        &self,
        log_directory: PathBuf,
        state_directory: PathBuf,
        workspace_store: PathBuf,
        startup: StartupDiagnostics,
    ) {
        let worker_state = self.clone();
        let worker_log_directory = log_directory.clone();
        let worker_state_directory = state_directory.clone();
        let worker_workspace_store = workspace_store.clone();
        let worker = std::thread::Builder::new()
            .name("axkdeck-server-startup".to_owned())
            .spawn(move || {
                let manager = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    ServerConnectionManager::initialize(
                        worker_log_directory.clone(),
                        worker_state_directory.clone(),
                        worker_workspace_store.clone(),
                        &startup,
                    )
                }))
                .map_err(|_| "axklib-server initialization panicked".to_owned())
                .and_then(|result| result)
                .unwrap_or_else(|error| {
                    log::error!("local axklib-server initialization failed: {error}");
                    ServerConnectionManager::unavailable(
                        error,
                        worker_log_directory,
                        worker_state_directory,
                        worker_workspace_store,
                    )
                });
                worker_state.complete(manager);
            });
        if let Err(error) = worker {
            let message = format!("start axklib-server initialization worker: {error}");
            log::error!("{message}");
            self.complete(ServerConnectionManager::unavailable(
                message,
                log_directory,
                state_directory,
                workspace_store,
            ));
        }
    }

    pub fn connection(&self) -> Result<Option<FrontendConnection>, String> {
        self.with_manager(ServerConnectionManager::connection)
    }

    pub fn settings(&self) -> RemoteServerSettingsView {
        self.with_manager(|manager| manager.settings())
    }

    pub fn configure_remote(
        &self,
        input: RemoteServerSettingsInput,
    ) -> Result<RemoteServerSettingsView, String> {
        self.with_manager(|manager| manager.configure_remote(input))
    }

    pub fn use_local(&self) -> Result<RemoteServerSettingsView, String> {
        self.with_manager(ServerConnectionManager::use_local)
    }

    fn complete(&self, manager: ServerConnectionManager) {
        let mut current = self
            .inner
            .manager
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if current.is_none() {
            *current = Some(manager);
            self.inner.ready.notify_all();
        }
    }

    fn with_manager<T>(&self, operation: impl FnOnce(&mut ServerConnectionManager) -> T) -> T {
        let mut current = self
            .inner
            .manager
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        while current.is_none() {
            current = self
                .inner
                .ready
                .wait(current)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
        }
        operation(current.as_mut().expect("server connection state completed"))
    }
}

impl ServerConnectionManager {
    pub fn initialize(
        log_directory: PathBuf,
        state_directory: PathBuf,
        workspace_store: PathBuf,
        startup: &StartupDiagnostics,
    ) -> Result<Self, String> {
        startup.record(StartupMilestone::CredentialLookupStarted);
        let settings = load_remote_settings();
        startup.set_credential_outcome(match &settings {
            Ok(Some(_)) => CredentialOutcome::Found,
            Ok(None) => CredentialOutcome::Missing,
            Err(_) => CredentialOutcome::Error,
        });
        startup.record(StartupMilestone::CredentialLookupCompleted);
        match settings {
            Ok(Some(settings)) => {
                startup.set_server_outcome(ServerOutcome::RemoteReady);
                Ok(Self {
                    sidecar: None,
                    connection: Some(frontend_connection(&settings)),
                    secure_storage_error: None,
                    log_directory,
                    state_directory,
                    workspace_store,
                })
            }
            Ok(None) => Self::local(
                log_directory,
                state_directory,
                workspace_store,
                None,
                Some(startup),
            ),
            Err(error) => Self::local(
                log_directory,
                state_directory,
                workspace_store,
                Some(error),
                Some(startup),
            ),
        }
    }

    fn local(
        log_directory: PathBuf,
        state_directory: PathBuf,
        workspace_store: PathBuf,
        secure_storage_error: Option<String>,
        startup: Option<&StartupDiagnostics>,
    ) -> Result<Self, String> {
        let sidecar = match startup {
            Some(startup) => ServerSidecar::launch_at_startup(
                &log_directory,
                &state_directory,
                &workspace_store,
                startup,
            )?,
            None => ServerSidecar::launch_if_available(
                &log_directory,
                &state_directory,
                &workspace_store,
            )?,
        };
        let connection = sidecar.as_ref().map(|server| server.connection().clone());
        Ok(Self {
            sidecar,
            connection,
            secure_storage_error,
            log_directory,
            state_directory,
            workspace_store,
        })
    }

    pub fn unavailable(
        error: String,
        log_directory: PathBuf,
        state_directory: PathBuf,
        workspace_store: PathBuf,
    ) -> Self {
        Self {
            sidecar: None,
            connection: None,
            secure_storage_error: Some(error),
            log_directory,
            state_directory,
            workspace_store,
        }
    }

    pub fn connection(&mut self) -> Result<Option<FrontendConnection>, String> {
        let failure = self
            .sidecar
            .as_ref()
            .and_then(|sidecar| sidecar.check_running().err());
        if let Some(error) = failure {
            self.connection = None;
            self.sidecar = None;
            self.secure_storage_error = Some(error.clone());
            return Err(error);
        }
        Ok(self.connection.clone())
    }

    pub fn settings(&self) -> RemoteServerSettingsView {
        let remote = self
            .connection
            .as_ref()
            .filter(|connection| connection.mode == "remote");
        RemoteServerSettingsView {
            mode: if remote.is_some() { "remote" } else { "local" }.to_owned(),
            base_url: remote.map(|connection| connection.base_url.clone()),
            token_configured: remote.is_some(),
            secure_storage_error: self.secure_storage_error.clone(),
        }
    }

    pub fn configure_remote(
        &mut self,
        input: RemoteServerSettingsInput,
    ) -> Result<RemoteServerSettingsView, String> {
        let settings = validate_remote_settings(input)?;
        save_remote_settings(&settings)?;
        self.connection = Some(frontend_connection(&settings));
        self.sidecar = None;
        self.secure_storage_error = None;
        Ok(self.settings())
    }

    pub fn use_local(&mut self) -> Result<RemoteServerSettingsView, String> {
        let sidecar = ServerSidecar::launch_if_available(
            &self.log_directory,
            &self.state_directory,
            &self.workspace_store,
        )?;
        let connection = sidecar.as_ref().map(|server| server.connection().clone());
        delete_remote_settings()?;
        self.connection = connection;
        self.sidecar = sidecar;
        self.secure_storage_error = None;
        Ok(self.settings())
    }
}

pub fn validate_remote_connection(
    input: RemoteServerSettingsInput,
) -> Result<FrontendConnection, String> {
    validate_remote_settings(input).map(|settings| frontend_connection(&settings))
}

fn validate_remote_settings(
    input: RemoteServerSettingsInput,
) -> Result<StoredRemoteSettings, String> {
    let base_url = input.base_url.trim();
    let mut parsed = Url::parse(base_url).map_err(|_| "enter a valid server URL".to_owned())?;
    if !matches!(parsed.scheme(), "http" | "https") {
        return Err("the server URL must use HTTPS".to_owned());
    }
    if !parsed.username().is_empty() || parsed.password().is_some() {
        return Err("the server URL must not contain credentials".to_owned());
    }
    if parsed.query().is_some() || parsed.fragment().is_some() {
        return Err("the server URL must not contain a query or fragment".to_owned());
    }
    let host = parsed
        .host()
        .ok_or_else(|| "the server URL must include a host".to_owned())?;
    if parsed.scheme() == "http" && !is_loopback_host(host) {
        return Err(
            "remote servers require HTTPS; plaintext HTTP is limited to loopback".to_owned(),
        );
    }
    if !matches!(parsed.path(), "" | "/" | "/api/v1" | "/api/v1/") {
        return Err("the server URL path must be empty or /api/v1".to_owned());
    }
    if input.bearer_token.len() < MINIMUM_TOKEN_BYTES
        || input.bearer_token.len() > MAXIMUM_TOKEN_BYTES
        || input.bearer_token.chars().any(char::is_whitespace)
    {
        return Err("the bearer token must contain 32 to 4096 non-whitespace bytes".to_owned());
    }
    parsed.set_path("/api/v1");
    parsed.set_query(None);
    parsed.set_fragment(None);
    Ok(StoredRemoteSettings {
        base_url: parsed.as_str().trim_end_matches('/').to_owned(),
        bearer_token: input.bearer_token,
    })
}

fn is_loopback_host(host: Host<&str>) -> bool {
    match host {
        Host::Domain(domain) => domain.eq_ignore_ascii_case("localhost"),
        Host::Ipv4(address) => address.is_loopback(),
        Host::Ipv6(address) => address.is_loopback(),
    }
}

fn frontend_connection(settings: &StoredRemoteSettings) -> FrontendConnection {
    FrontendConnection {
        base_url: settings.base_url.clone(),
        bearer_token: settings.bearer_token.clone(),
        mode: "remote".to_owned(),
    }
}

fn credential_entry() -> Result<keyring::Entry, String> {
    keyring::Entry::new(KEYRING_SERVICE, KEYRING_USER)
        .map_err(|error| format!("access protected server settings: {error}"))
}

fn load_remote_settings() -> Result<Option<StoredRemoteSettings>, String> {
    let entry = credential_entry()?;
    let encoded = match entry.get_password() {
        Ok(value) => value,
        Err(keyring::Error::NoEntry) => return Ok(None),
        Err(error) => return Err(format!("read protected server settings: {error}")),
    };
    serde_json::from_str(&encoded)
        .map(Some)
        .map_err(|_| "protected server settings are invalid; clear and reconfigure them".to_owned())
}

fn save_remote_settings(settings: &StoredRemoteSettings) -> Result<(), String> {
    let encoded = serde_json::to_string(settings)
        .map_err(|error| format!("encode protected server settings: {error}"))?;
    credential_entry()?
        .set_password(&encoded)
        .map_err(|error| format!("write protected server settings: {error}"))
}

fn delete_remote_settings() -> Result<(), String> {
    match credential_entry()?.delete_credential() {
        Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
        Err(error) => Err(format!("delete protected server settings: {error}")),
    }
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;
    use std::sync::mpsc;
    use std::time::Duration;

    use super::{
        RemoteServerSettingsInput, ServerConnectionManager, ServerConnectionState,
        validate_remote_settings,
    };

    fn settings(base_url: &str) -> RemoteServerSettingsInput {
        RemoteServerSettingsInput {
            base_url: base_url.to_owned(),
            bearer_token: "0123456789abcdef0123456789abcdef".to_owned(),
        }
    }

    #[test]
    fn canonicalizes_remote_api_urls() {
        let https = validate_remote_settings(settings("https://sampler.example.test"))
            .expect("validate HTTPS endpoint");
        assert_eq!(https.base_url, "https://sampler.example.test/api/v1");

        let loopback = validate_remote_settings(settings("http://127.0.0.1:7300/api/v1/"))
            .expect("validate loopback endpoint");
        assert_eq!(loopback.base_url, "http://127.0.0.1:7300/api/v1");
    }

    #[test]
    fn rejects_unsafe_or_ambiguous_remote_urls() {
        for base_url in [
            "http://sampler.example.test",
            "ftp://sampler.example.test",
            "https://user:secret@sampler.example.test",
            "https://sampler.example.test/api/v2",
            "https://sampler.example.test/api/v1?token=secret",
            "https://sampler.example.test/api/v1#fragment",
        ] {
            assert!(
                validate_remote_settings(settings(base_url)).is_err(),
                "unexpectedly accepted {base_url}"
            );
        }
    }

    #[test]
    fn rejects_weak_or_malformed_bearer_tokens() {
        let mut weak = settings("https://sampler.example.test");
        weak.bearer_token = "short".to_owned();
        assert!(validate_remote_settings(weak).is_err());

        let mut whitespace = settings("https://sampler.example.test");
        whitespace.bearer_token = "0123456789abcdef 0123456789abcdef".to_owned();
        assert!(validate_remote_settings(whitespace).is_err());
    }

    #[test]
    fn pending_connection_state_releases_waiters_when_initialization_completes() {
        let state = ServerConnectionState::pending();
        let waiter_state = state.clone();
        let (started_sender, started_receiver) = mpsc::channel();
        let (result_sender, result_receiver) = mpsc::channel();
        let waiter = std::thread::spawn(move || {
            started_sender.send(()).expect("report waiter start");
            result_sender
                .send(waiter_state.settings())
                .expect("report completed settings");
        });

        started_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("waiter started");
        assert!(
            result_receiver
                .recv_timeout(Duration::from_millis(20))
                .is_err()
        );

        state.complete(ServerConnectionManager::unavailable(
            "sidecar unavailable".to_owned(),
            PathBuf::from("logs"),
            PathBuf::from("state"),
            PathBuf::from("config/workspaces.json"),
        ));
        let settings = result_receiver
            .recv_timeout(Duration::from_secs(1))
            .expect("waiter released");
        assert_eq!(settings.mode, "local");
        assert_eq!(
            settings.secure_storage_error.as_deref(),
            Some("sidecar unavailable")
        );
        waiter.join().expect("join waiter");
    }
}
