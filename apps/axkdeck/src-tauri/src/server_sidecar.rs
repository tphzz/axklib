use std::ffi::OsString;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use url::{Host, Url};

#[path = "server_sidecar_runtime.rs"]
mod runtime;
#[path = "server_sidecar_shutdown.rs"]
mod shutdown;
use shutdown::{request_shutdown, signal_shutdown};

use crate::startup_diagnostics::{ServerOutcome, StartupDiagnostics, StartupMilestone};
use runtime::PrivateRuntimeDirectory;

const CONNECTION_SCHEMA_VERSION: u32 = 1;
const API_VERSION: &str = "v1";
const LOG_FILE_SIZE: u64 = 5 * 1024 * 1024;
const RETAINED_LOG_FILES: usize = 3;
const ALLOWED_ORIGINS: [&str; 3] = [
    "http://localhost:5173",
    "tauri://localhost",
    "http://tauri.localhost",
];

#[cfg(windows)]
fn suppress_child_console(command: &mut Command) {
    use std::os::windows::process::CommandExt;

    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.creation_flags(CREATE_NO_WINDOW);
}

#[cfg(not(windows))]
fn suppress_child_console(_command: &mut Command) {}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ConnectionFile {
    schema_version: u32,
    api_version: String,
    pid: u32,
    base_url: String,
    websocket_url: String,
    bearer_token: String,
    semantic_version: String,
    source_identity: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FrontendConnection {
    pub base_url: String,
    pub bearer_token: String,
    pub mode: String,
}

pub fn create_workspace(
    connection: &FrontendConnection,
    path: &Path,
    display_name: &str,
    writable: bool,
    revision: u64,
) -> Result<(), String> {
    if connection.mode != "local" {
        return Err("native workspace selection is available only for the local server".to_owned());
    }
    let authority = connection
        .base_url
        .strip_prefix("http://")
        .and_then(|value| value.strip_suffix("/api/v1"))
        .ok_or_else(|| "local server URL is not canonical loopback HTTP".to_owned())?;
    let body = serde_json::to_vec(&serde_json::json!({
        "displayName": display_name,
        "path": path,
        "writable": writable,
        "revision": revision,
    }))
    .map_err(|error| format!("encode workspace request: {error}"))?;
    let mut stream = TcpStream::connect(authority)
        .map_err(|error| format!("connect to local axklib-server: {error}"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .map_err(|error| format!("set workspace response timeout: {error}"))?;
    let request = format!(
        "POST /api/v1/workspaces HTTP/1.1\r\nHost: {authority}\r\nAuthorization: Bearer {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        connection.bearer_token,
        body.len()
    );
    stream
        .write_all(request.as_bytes())
        .and_then(|()| stream.write_all(&body))
        .map_err(|error| format!("send workspace request: {error}"))?;
    let mut response = Vec::new();
    stream
        .read_to_end(&mut response)
        .map_err(|error| format!("read workspace response: {error}"))?;
    require_http_status(&response, 201, "create workspace")
}

pub struct ServerSidecar {
    child: Mutex<Option<Child>>,
    log_threads: Vec<JoinHandle<()>>,
    _runtime_directory: PrivateRuntimeDirectory,
    connection: FrontendConnection,
}

impl ServerSidecar {
    pub fn launch_if_available(
        log_directory: &Path,
        state_directory: &Path,
        workspace_store: &Path,
    ) -> Result<Option<Self>, String> {
        Self::launch_if_available_inner(log_directory, state_directory, workspace_store, None)
    }

    pub fn launch_at_startup(
        log_directory: &Path,
        state_directory: &Path,
        workspace_store: &Path,
        startup: &StartupDiagnostics,
    ) -> Result<Option<Self>, String> {
        Self::launch_if_available_inner(
            log_directory,
            state_directory,
            workspace_store,
            Some(startup),
        )
    }

    fn launch_if_available_inner(
        log_directory: &Path,
        state_directory: &Path,
        workspace_store: &Path,
        startup: Option<&StartupDiagnostics>,
    ) -> Result<Option<Self>, String> {
        if let Some(startup) = startup {
            startup.record(StartupMilestone::SidecarStartupStarted);
        }
        if std::env::var("AXKDECK_HTTP_SERVER").is_ok_and(|value| {
            matches!(
                value.trim().to_ascii_lowercase().as_str(),
                "0" | "false" | "no"
            )
        }) {
            complete_sidecar_startup(startup, ServerOutcome::Disabled);
            return Ok(None);
        }
        let Some(binary) = server_binary() else {
            complete_sidecar_startup(startup, ServerOutcome::BinaryUnavailable);
            return Ok(None);
        };
        match Self::launch(
            &binary,
            log_directory,
            state_directory,
            workspace_store,
            startup,
        ) {
            Ok(sidecar) => {
                complete_sidecar_startup(startup, ServerOutcome::LocalReady);
                Ok(Some(sidecar))
            }
            Err(error) => {
                complete_sidecar_startup(startup, ServerOutcome::Failed);
                Err(error)
            }
        }
    }

    fn launch(
        binary: &Path,
        log_directory: &Path,
        state_directory: &Path,
        workspace_store: &Path,
        startup: Option<&StartupDiagnostics>,
    ) -> Result<Self, String> {
        prepare_persistent_state_directory(state_directory)?;
        let runtime_directory = PrivateRuntimeDirectory::create(&std::env::temp_dir())?;
        record_startup(startup, StartupMilestone::SidecarStatePrepared);
        let connection_path = runtime_directory.connection_path();
        let mut arguments = sidecar_arguments(state_directory, workspace_store, &connection_path);
        if cfg!(windows) {
            let directory = binary.parent().ok_or("server binary has no directory")?;
            let packaged = directory.join("axklib_helper.exe");
            let helper = if packaged.is_file() {
                packaged
            } else {
                directory.join("../../devices/axklib_helper.exe")
            };
            let helper = helper.canonicalize().map_err(|error| {
                format!("device helper is missing at {}: {error}", helper.display())
            })?;
            arguments.push("--device-helper".into());
            arguments.push(helper.into_os_string());
        }
        let mut command = Command::new(binary);
        command
            .args(arguments)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        if let Ok(level) = std::env::var("AXKDECK_LOG_LEVEL") {
            command.env("AXKLIB_SERVER_LOG_LEVEL", level);
        }
        suppress_child_console(&mut command);
        let mut child = command
            .spawn()
            .map_err(|error| format!("start axklib-server: {error}"))?;
        record_startup(startup, StartupMilestone::SidecarSpawned);
        let log_threads = match capture_child_logs(&mut child, log_directory) {
            Ok(threads) => threads,
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(error);
            }
        };

        let metadata =
            match wait_for_connection(&runtime_directory, &mut child, Duration::from_secs(10)) {
                Ok(metadata) => metadata,
                Err(error) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    join_log_threads(log_threads);
                    return Err(error);
                }
            };
        record_startup(startup, StartupMilestone::SidecarReadinessReceived);
        if let Err(error) = validate_connection(
            &metadata,
            child.id(),
            env!("AXKDECK_SEMANTIC_VERSION"),
            env!("AXKDECK_SOURCE_IDENTITY"),
        )
        .and_then(|()| runtime_directory.remove_connection())
        {
            let _ = child.kill();
            let _ = child.wait();
            join_log_threads(log_threads);
            return Err(error);
        }
        record_startup(startup, StartupMilestone::SidecarConnectionValidated);
        let connection = FrontendConnection {
            base_url: metadata.base_url,
            bearer_token: metadata.bearer_token,
            mode: "local".to_owned(),
        };
        Ok(Self {
            child: Mutex::new(Some(child)),
            log_threads,
            _runtime_directory: runtime_directory,
            connection,
        })
    }

    pub fn connection(&self) -> &FrontendConnection {
        &self.connection
    }

    pub fn check_running(&self) -> Result<(), String> {
        let mut child = self
            .child
            .lock()
            .map_err(|_| "axklib-server process state is unavailable".to_owned())?;
        let child = child
            .as_mut()
            .ok_or_else(|| "axklib-server process is no longer owned by axkdeck".to_owned())?;
        match child
            .try_wait()
            .map_err(|error| format!("inspect axklib-server process: {error}"))?
        {
            Some(status) => Err(format!(
                "axklib-server process {} exited unexpectedly: {status}",
                child.id()
            )),
            None => Ok(()),
        }
    }
}

fn record_startup(startup: Option<&StartupDiagnostics>, milestone: StartupMilestone) {
    if let Some(startup) = startup {
        startup.record(milestone);
    }
}

fn complete_sidecar_startup(startup: Option<&StartupDiagnostics>, outcome: ServerOutcome) {
    if let Some(startup) = startup {
        startup.set_server_outcome(outcome);
        startup.record(StartupMilestone::SidecarStartupCompleted);
    }
}

impl Drop for ServerSidecar {
    fn drop(&mut self) {
        if let Ok(mut child) = self.child.lock() {
            if let Some(mut child) = child.take() {
                let path = self._runtime_directory.connection_path();
                loop {
                    if child.try_wait().ok().flatten().is_some() {
                        break;
                    }
                    let signalled = signal_shutdown(&path, &self.connection);
                    let requested = request_shutdown(&self.connection);
                    if signalled.is_ok() || requested.is_ok() {
                        break;
                    }
                    log::warn!(
                        "{}",
                        serde_json::json!({"event":"sidecar_shutdown_pending", "source":"application",
                        "level":"warning", "message":"graceful shutdown not acknowledged; retaining sidecar ownership and retrying"})
                    );
                    std::thread::sleep(Duration::from_secs(1));
                }
                // The server cancels queued work and drains any write/restoration.
                // A timeout must never kill it in the middle of a physical write.
                let _ = child.wait();
            }
        }
        join_log_threads(std::mem::take(&mut self.log_threads));
    }
}

struct RotatingLogWriter {
    path: PathBuf,
    file: File,
    size: u64,
    maximum_size: u64,
    retained_files: usize,
}

impl RotatingLogWriter {
    fn open(directory: &Path, maximum_size: u64, retained_files: usize) -> Result<Self, String> {
        std::fs::create_dir_all(directory)
            .map_err(|error| format!("create sidecar log directory: {error}"))?;
        let path = directory.join("axklib-server.log");
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .map_err(|error| format!("open sidecar log: {error}"))?;
        let size = file
            .metadata()
            .map_err(|error| format!("inspect sidecar log: {error}"))?
            .len();
        Ok(Self {
            path,
            file,
            size,
            maximum_size,
            retained_files: retained_files.max(1),
        })
    }

    fn write_line(&mut self, stream: &str, line: &str) -> Result<(), String> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        let entry = format!("{timestamp} [{stream}] {line}\n");
        let entry_size = u64::try_from(entry.len()).unwrap_or(u64::MAX);
        if self.size > 0 && self.size.saturating_add(entry_size) > self.maximum_size {
            self.rotate()?;
        }
        self.file
            .write_all(entry.as_bytes())
            .and_then(|()| self.file.flush())
            .map_err(|error| format!("write sidecar log: {error}"))?;
        self.size = self.size.saturating_add(entry_size);
        Ok(())
    }

    fn rotate(&mut self) -> Result<(), String> {
        self.file
            .flush()
            .map_err(|error| format!("flush sidecar log before rotation: {error}"))?;
        if self.retained_files > 1 {
            let _ = std::fs::remove_file(numbered_log_path(&self.path, self.retained_files - 1));
        }
        for index in (1..self.retained_files.saturating_sub(1)).rev() {
            let source = numbered_log_path(&self.path, index);
            let destination = numbered_log_path(&self.path, index + 1);
            if !source.exists() {
                continue;
            }
            let _ = std::fs::remove_file(&destination);
            std::fs::rename(&source, &destination)
                .map_err(|error| format!("rotate sidecar log archive: {error}"))?;
        }
        if self.retained_files > 1 {
            let destination = numbered_log_path(&self.path, 1);
            let _ = std::fs::remove_file(&destination);
            std::fs::rename(&self.path, destination)
                .map_err(|error| format!("rotate active sidecar log: {error}"))?;
        } else {
            std::fs::remove_file(&self.path)
                .map_err(|error| format!("replace active sidecar log: {error}"))?;
        }
        self.file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .map_err(|error| format!("reopen sidecar log: {error}"))?;
        self.size = 0;
        Ok(())
    }
}

fn numbered_log_path(path: &Path, index: usize) -> PathBuf {
    let mut filename = path.as_os_str().to_os_string();
    filename.push(format!(".{index}"));
    PathBuf::from(filename)
}

fn capture_child_logs(
    child: &mut Child,
    log_directory: &Path,
) -> Result<Vec<JoinHandle<()>>, String> {
    let writer = Arc::new(Mutex::new(RotatingLogWriter::open(
        log_directory,
        LOG_FILE_SIZE,
        RETAINED_LOG_FILES,
    )?));
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "axklib-server stdout pipe is unavailable".to_owned())?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| "axklib-server stderr pipe is unavailable".to_owned())?;
    Ok(vec![
        spawn_log_reader("stdout", stdout, Arc::clone(&writer)),
        spawn_log_reader("stderr", stderr, writer),
    ])
}

fn spawn_log_reader<R>(
    stream: &'static str,
    reader: R,
    writer: Arc<Mutex<RotatingLogWriter>>,
) -> JoinHandle<()>
where
    R: Read + Send + 'static,
{
    std::thread::spawn(move || {
        let result = crate::diagnostic_format::read_server_records(reader, |level, line| {
            if level.filter() <= crate::configured_log_level() {
                eprintln!("axklib-server[{stream}]: {line}");
            }
            if level.filter() > crate::diagnostic_capture::retained_filter() {
                return true;
            }
            if let Ok(mut writer) = writer.lock() {
                if let Err(error) = writer.write_line(stream, line) {
                    log::error!(
                        "{}",
                        serde_json::json!({"event":"server_log_write_failed", "source":"application",
                        "level":"error", "stream":stream, "message":error.to_string()})
                    );
                    return false;
                }
            }
            true
        });
        if let Err(error) = result {
            log::error!(
                "{}",
                serde_json::json!({"event":"server_log_read_failed", "source":"application",
                "level":"error", "stream":stream, "message":error.to_string()})
            );
        }
    })
}

fn join_log_threads(threads: Vec<JoinHandle<()>>) {
    for thread in threads {
        let _ = thread.join();
    }
}

fn require_http_status(response: &[u8], expected: u16, operation: &str) -> Result<(), String> {
    let document = String::from_utf8_lossy(response);
    let (headers, body) = document.split_once("\r\n\r\n").unwrap_or((&document, ""));
    let status = headers
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|value| value.parse::<u16>().ok())
        .ok_or_else(|| {
            format!("axklib-server returned a malformed response while attempting to {operation}")
        })?;
    if status == expected {
        return Ok(());
    }
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(body) {
        if let Some(error) = value.get("error") {
            let message = error.get("message").and_then(serde_json::Value::as_str);
            let request_id = error.get("requestId").and_then(serde_json::Value::as_str);
            if let Some(message) = message {
                return Err(match request_id {
                    Some(request_id) => format!(
                        "axklib-server could not {operation}: {message} (request {request_id})"
                    ),
                    None => format!("axklib-server could not {operation}: {message}"),
                });
            }
        }
    }
    Err(format!(
        "axklib-server could not {operation}: HTTP status {status}"
    ))
}

fn prepare_persistent_state_directory(state_directory: &Path) -> Result<(), String> {
    std::fs::create_dir_all(state_directory)
        .map_err(|error| format!("create persistent sidecar state directory: {error}"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(state_directory, std::fs::Permissions::from_mode(0o700))
            .map_err(|error| format!("secure persistent sidecar state directory: {error}"))?;
    }
    Ok(())
}
fn sidecar_arguments(
    state_directory: &Path,
    workspace_store: &Path,
    connection_path: &Path,
) -> Vec<OsString> {
    let mut arguments = vec![
        "--port".into(),
        "0".into(),
        "--enable-devices".into(),
        "--state-directory".into(),
        state_directory.as_os_str().into(),
        "--workspace-store".into(),
        workspace_store.as_os_str().into(),
        "--connection-file".into(),
        connection_path.as_os_str().into(),
        "--parent-pid".into(),
        std::process::id().to_string().into(),
    ];
    for origin in ALLOWED_ORIGINS {
        arguments.push("--allow-origin".into());
        arguments.push(origin.into());
    }
    arguments
}

fn wait_for_connection(
    runtime_directory: &PrivateRuntimeDirectory,
    child: &mut Child,
    timeout: Duration,
) -> Result<ConnectionFile, String> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Some(status) = child
            .try_wait()
            .map_err(|error| format!("inspect axklib-server process: {error}"))?
        {
            return Err(format!("axklib-server exited before readiness: {status}"));
        }
        if runtime_directory.connection_path().is_file() {
            let document = runtime_directory.read_connection()?;
            return serde_json::from_slice(&document)
                .map_err(|error| format!("parse sidecar connection file: {error}"));
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    Err(format!(
        "axklib-server did not publish readiness metadata within {} ms",
        timeout.as_millis()
    ))
}

fn parse_loopback_endpoint(value: &str, scheme: &str, path: &str) -> Result<Url, String> {
    let parsed = Url::parse(value).map_err(|error| format!("parse sidecar endpoint: {error}"))?;
    if parsed.scheme() != scheme
        || parsed.host() != Some(Host::Ipv4(std::net::Ipv4Addr::LOCALHOST))
        || parsed.port().is_none()
        || parsed.path() != path
        || !parsed.username().is_empty()
        || parsed.password().is_some()
        || parsed.query().is_some()
        || parsed.fragment().is_some()
    {
        return Err("sidecar endpoint is not canonical loopback metadata".to_owned());
    }
    Ok(parsed)
}

fn validate_connection(
    connection: &ConnectionFile,
    child_pid: u32,
    semantic_version: &str,
    source_identity: &str,
) -> Result<(), String> {
    if connection.schema_version != CONNECTION_SCHEMA_VERSION
        || connection.api_version != API_VERSION
    {
        return Err(format!(
            "unsupported axklib-server protocol {} / {}",
            connection.schema_version, connection.api_version
        ));
    }
    if connection.pid != child_pid {
        return Err("sidecar connection PID does not match the child process".to_owned());
    }
    let base_url = parse_loopback_endpoint(&connection.base_url, "http", "/api/v1")?;
    let websocket_url = parse_loopback_endpoint(&connection.websocket_url, "ws", "/api/v1/events")?;
    if base_url.port() != websocket_url.port()
        || connection.bearer_token.len() < 32
        || connection.semantic_version.is_empty()
        || connection.source_identity.is_empty()
    {
        return Err("sidecar connection metadata is incomplete or non-loopback".to_owned());
    }
    if connection.semantic_version != semantic_version
        || connection.source_identity != source_identity
    {
        return Err(format!(
            "axklib-server build identity does not match axkdeck: expected {semantic_version} / {source_identity}, found {} / {}",
            connection.semantic_version, connection.source_identity
        ));
    }
    Ok(())
}

fn server_binary() -> Option<PathBuf> {
    if let Some(configured) = std::env::var_os("AXKLIB_SERVER_BINARY") {
        let path = PathBuf::from(configured);
        return path.is_file().then_some(path);
    }
    let filename = if cfg!(windows) {
        "axklib-server.exe"
    } else {
        "axklib-server"
    };
    if let Ok(executable) = std::env::current_exe() {
        if let Some(directory) = executable.parent() {
            let path = directory.join(filename);
            if path.is_file() {
                return Some(path);
            }
        }
    }
    let development = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../build/native/release/apps/server")
        .join(filename);
    development.is_file().then_some(development)
}

#[cfg(test)]
#[path = "server_sidecar_tests.rs"]
mod tests;
