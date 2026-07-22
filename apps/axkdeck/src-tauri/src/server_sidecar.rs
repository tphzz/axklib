use std::ffi::OsString;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

const CONNECTION_SCHEMA_VERSION: u32 = 1;
const API_VERSION: &str = "v1";
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(3);
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
    state_directory: PathBuf,
    connection: FrontendConnection,
    shutdown_timeout: Duration,
}

impl ServerSidecar {
    pub fn launch_if_available(log_directory: &Path) -> Result<Option<Self>, String> {
        if std::env::var("AXKDECK_HTTP_SERVER").is_ok_and(|value| {
            matches!(
                value.trim().to_ascii_lowercase().as_str(),
                "0" | "false" | "no"
            )
        }) {
            return Ok(None);
        }
        let Some(binary) = server_binary() else {
            return Ok(None);
        };
        Self::launch(&binary, log_directory).map(Some)
    }

    fn launch(binary: &Path, log_directory: &Path) -> Result<Self, String> {
        let state_directory =
            std::env::temp_dir().join(format!("axkdeck-server-{}", std::process::id()));
        prepare_state_directory(&state_directory)?;
        let connection_path = state_directory.join("connection.json");
        let arguments = sidecar_arguments(&state_directory, &connection_path);
        let mut command = Command::new(binary);
        command
            .args(arguments)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        suppress_child_console(&mut command);
        let mut child = command
            .spawn()
            .map_err(|error| format!("start axklib-server: {error}"))?;
        let log_threads = match capture_child_logs(&mut child, log_directory) {
            Ok(threads) => threads,
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                let _ = std::fs::remove_dir_all(&state_directory);
                return Err(error);
            }
        };

        let metadata =
            match wait_for_connection(&connection_path, &mut child, Duration::from_secs(10)) {
                Ok(metadata) => metadata,
                Err(error) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    join_log_threads(log_threads);
                    let _ = std::fs::remove_dir_all(&state_directory);
                    return Err(error);
                }
            };
        if let Err(error) = validate_connection(
            &metadata,
            child.id(),
            env!("AXKDECK_SEMANTIC_VERSION"),
            env!("AXKDECK_SOURCE_IDENTITY"),
        )
        .and_then(|()| {
            std::fs::remove_file(&connection_path)
                .map_err(|error| format!("remove consumed sidecar connection file: {error}"))
        }) {
            let _ = child.kill();
            let _ = child.wait();
            join_log_threads(log_threads);
            let _ = std::fs::remove_dir_all(&state_directory);
            return Err(error);
        }
        let connection = FrontendConnection {
            base_url: metadata.base_url,
            bearer_token: metadata.bearer_token,
            mode: "local".to_owned(),
        };
        Ok(Self {
            child: Mutex::new(Some(child)),
            log_threads,
            state_directory,
            connection,
            shutdown_timeout: SHUTDOWN_TIMEOUT,
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

impl Drop for ServerSidecar {
    fn drop(&mut self) {
        if let Ok(mut child) = self.child.lock() {
            if let Some(mut child) = child.take() {
                let _ = request_shutdown(&self.connection);
                let deadline = Instant::now() + self.shutdown_timeout;
                while Instant::now() < deadline {
                    if child.try_wait().is_ok_and(|status| status.is_some()) {
                        break;
                    }
                    std::thread::sleep(Duration::from_millis(20));
                }
                if child.try_wait().is_ok_and(|status| status.is_none()) {
                    let _ = child.kill();
                }
                let _ = child.wait();
            }
        }
        join_log_threads(std::mem::take(&mut self.log_threads));
        let _ = std::fs::remove_dir_all(&self.state_directory);
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
        for result in BufReader::new(reader).lines() {
            let line = match result {
                Ok(line) => line,
                Err(error) => {
                    eprintln!("read axklib-server {stream}: {error}");
                    break;
                }
            };
            #[cfg(debug_assertions)]
            if log::log_enabled!(log::Level::Debug) {
                eprintln!("axklib-server[{stream}]: {line}");
            }
            if let Ok(mut writer) = writer.lock() {
                if let Err(error) = writer.write_line(stream, &line) {
                    eprintln!("{error}");
                    break;
                }
            }
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

fn prepare_state_directory(state_directory: &Path) -> Result<(), String> {
    if state_directory.exists() {
        std::fs::remove_dir_all(state_directory)
            .map_err(|error| format!("remove stale sidecar state: {error}"))?;
    }
    std::fs::create_dir_all(state_directory)
        .map_err(|error| format!("create sidecar state directory: {error}"))
}

fn request_shutdown(connection: &FrontendConnection) -> Result<(), String> {
    let authority = connection
        .base_url
        .strip_prefix("http://")
        .and_then(|value| value.strip_suffix("/api/v1"))
        .ok_or_else(|| "sidecar base URL is not canonical loopback HTTP".to_owned())?;
    let mut stream = TcpStream::connect(authority)
        .map_err(|error| format!("connect for sidecar shutdown: {error}"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .map_err(|error| format!("set sidecar shutdown read timeout: {error}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(2)))
        .map_err(|error| format!("set sidecar shutdown write timeout: {error}"))?;
    let request = format!(
        "POST /api/v1/system/shutdown HTTP/1.1\r\nHost: {authority}\r\nAuthorization: Bearer {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        connection.bearer_token
    );
    stream
        .write_all(request.as_bytes())
        .map_err(|error| format!("write sidecar shutdown request: {error}"))?;
    let mut response = Vec::new();
    stream
        .read_to_end(&mut response)
        .map_err(|error| format!("read sidecar shutdown response: {error}"))?;
    if !response.starts_with(b"HTTP/1.1 202 ") {
        return Err("axklib-server rejected the sidecar shutdown request".to_owned());
    }
    Ok(())
}

fn sidecar_arguments(state_directory: &Path, connection_path: &Path) -> Vec<OsString> {
    let mut arguments = vec![
        "--port".into(),
        "0".into(),
        "--state-directory".into(),
        state_directory.as_os_str().into(),
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
    path: &Path,
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
        if path.is_file() {
            let document = std::fs::read(path)
                .map_err(|error| format!("read sidecar connection file: {error}"))?;
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
    if !connection.base_url.starts_with("http://127.0.0.1:")
        || !connection.websocket_url.starts_with("ws://127.0.0.1:")
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
mod tests {
    use super::{
        ConnectionFile, FrontendConnection, RotatingLogWriter, ServerSidecar,
        prepare_state_directory, request_shutdown, require_http_status, sidecar_arguments,
        validate_connection, wait_for_connection,
    };
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::path::PathBuf;
    use std::process::{Child, Command, Stdio};
    use std::sync::Mutex;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    fn temporary_directory(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "axkdeck-sidecar-{name}-{}-{nonce}",
            std::process::id()
        ))
    }

    #[cfg(unix)]
    fn waiting_child() -> Child {
        Command::new("sh")
            .args(["-c", "sleep 60"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn waiting child")
    }

    #[cfg(windows)]
    fn waiting_child() -> Child {
        Command::new("cmd")
            .args(["/C", "ping -n 60 127.0.0.1 >NUL"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn waiting child")
    }

    #[cfg(unix)]
    fn exiting_child() -> Child {
        Command::new("sh")
            .args(["-c", "exit 7"])
            .spawn()
            .expect("spawn exiting child")
    }

    #[cfg(windows)]
    fn exiting_child() -> Child {
        Command::new("cmd")
            .args(["/C", "exit 7"])
            .spawn()
            .expect("spawn exiting child")
    }

    fn connection(pid: u32) -> ConnectionFile {
        ConnectionFile {
            schema_version: 1,
            api_version: "v1".to_owned(),
            pid,
            base_url: "http://127.0.0.1:7300/api/v1".to_owned(),
            websocket_url: "ws://127.0.0.1:7300/api/v1/events".to_owned(),
            bearer_token: "0123456789abcdef0123456789abcdef".to_owned(),
            semantic_version: "0.0.0".to_owned(),
            source_identity: "main-1234567".to_owned(),
        }
    }

    #[test]
    fn sidecar_arguments_never_expose_the_bearer_token() {
        let arguments = sidecar_arguments(
            std::path::Path::new("state"),
            std::path::Path::new("state/connection.json"),
        );
        let rendered = arguments
            .iter()
            .map(|value| value.to_string_lossy())
            .collect::<Vec<_>>()
            .join(" ");
        assert!(!rendered.contains("--token"));
        assert!(!rendered.contains("--config"));
        assert!(rendered.contains("--connection-file"));
        assert!(rendered.contains("--port 0"));
        assert!(rendered.contains(&format!("--parent-pid {}", std::process::id())));
    }

    #[test]
    fn sidecar_arguments_allow_tauri_development_and_packaged_origins() {
        let arguments = sidecar_arguments(
            std::path::Path::new("state"),
            std::path::Path::new("state/connection.json"),
        );
        let rendered = arguments
            .iter()
            .map(|value| value.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        let allowed_origins = rendered
            .windows(2)
            .filter(|pair| pair[0] == "--allow-origin")
            .map(|pair| pair[1].as_str())
            .collect::<Vec<_>>();

        assert_eq!(
            allowed_origins,
            [
                "http://localhost:5173",
                "tauri://localhost",
                "http://tauri.localhost"
            ]
        );
    }

    #[test]
    fn connection_metadata_must_match_the_loopback_child() {
        let metadata = connection(42);
        assert!(validate_connection(&metadata, 42, "0.0.0", "main-1234567").is_ok());
        assert!(validate_connection(&metadata, 43, "0.0.0", "main-1234567").is_err());

        let mut wrong_protocol = metadata.clone();
        wrong_protocol.api_version = "v2".to_owned();
        assert!(validate_connection(&wrong_protocol, 42, "0.0.0", "main-1234567").is_err());

        let mut weak_token = metadata.clone();
        weak_token.bearer_token = "short".to_owned();
        assert!(validate_connection(&weak_token, 42, "0.0.0", "main-1234567").is_err());

        let mut non_loopback = metadata;
        non_loopback.base_url = "http://192.0.2.1:7300/api/v1".to_owned();
        assert!(validate_connection(&non_loopback, 42, "0.0.0", "main-1234567").is_err());

        let connection = connection(42);
        assert!(validate_connection(&connection, 42, "1.0.0", "main-1234567").is_err());
        assert!(validate_connection(&connection, 42, "0.0.0", "other-7654321").is_err());
    }

    #[test]
    fn stale_state_is_removed_before_launch() {
        let directory = temporary_directory("stale");
        std::fs::create_dir_all(&directory).expect("create stale state");
        std::fs::write(directory.join("connection.json"), b"stale secret")
            .expect("write stale connection file");

        prepare_state_directory(&directory).expect("prepare sidecar state");

        assert!(directory.is_dir());
        assert!(!directory.join("connection.json").exists());
        std::fs::remove_dir_all(directory).expect("remove test state");
    }

    #[test]
    fn readiness_reports_timeout_and_child_exit_separately() {
        let directory = temporary_directory("readiness");
        std::fs::create_dir_all(&directory).expect("create readiness state");
        let connection_path = directory.join("connection.json");

        let mut waiting = waiting_child();
        let timeout =
            wait_for_connection(&connection_path, &mut waiting, Duration::from_millis(30))
                .expect_err("missing connection file must time out");
        assert!(timeout.contains("within 30 ms"), "{timeout}");
        waiting.kill().expect("kill waiting child");
        waiting.wait().expect("reap waiting child");

        let mut exiting = exiting_child();
        let exited = wait_for_connection(&connection_path, &mut exiting, Duration::from_secs(1))
            .expect_err("exited child must be reported");
        assert!(exited.contains("exited before readiness"), "{exited}");
        exiting.wait().expect("reap exiting child");
        std::fs::remove_dir_all(directory).expect("remove readiness state");
    }

    #[test]
    fn crashed_sidecar_is_reported_and_drop_reaps_the_child_and_state() {
        let directory = temporary_directory("crash");
        std::fs::create_dir_all(&directory).expect("create crash state");
        std::fs::write(directory.join("upload.partial"), b"partial").expect("write staged partial");
        let mut child = exiting_child();
        let pid = child.id();
        child.wait().expect("wait for child crash");
        let sidecar = ServerSidecar {
            child: Mutex::new(Some(child)),
            log_threads: Vec::new(),
            state_directory: directory.clone(),
            connection: FrontendConnection {
                base_url: "http://127.0.0.1:1/api/v1".to_owned(),
                bearer_token: "0123456789abcdef0123456789abcdef".to_owned(),
                mode: "local".to_owned(),
            },
            shutdown_timeout: Duration::from_millis(20),
        };

        let error = sidecar
            .check_running()
            .expect_err("crashed child must be reported");
        assert!(error.contains(&pid.to_string()), "{error}");
        drop(sidecar);
        assert!(!directory.exists());
    }

    #[test]
    fn sidecar_shutdown_uses_authenticated_loopback_http() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind test listener");
        let address = listener.local_addr().expect("inspect test listener");
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept shutdown request");
            let mut request = [0_u8; 2048];
            let length = stream.read(&mut request).expect("read shutdown request");
            let request = String::from_utf8_lossy(&request[..length]);
            assert!(request.starts_with("POST /api/v1/system/shutdown HTTP/1.1\r\n"));
            assert!(request.contains("Authorization: Bearer test-token\r\n"));
            stream
                .write_all(
                    b"HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                )
                .expect("write shutdown response");
        });
        let connection = FrontendConnection {
            base_url: format!("http://{address}/api/v1"),
            bearer_token: "test-token".to_owned(),
            mode: "local".to_owned(),
        };
        request_shutdown(&connection).expect("request graceful shutdown");
        server.join().expect("join test server");
    }

    #[test]
    fn workspace_errors_preserve_server_message_and_request_id() {
        let response = b"HTTP/1.1 422 Unprocessable Entity\r\nContent-Type: application/json\r\n\r\n{\"error\":{\"message\":\"the selected directory is not accessible\",\"requestId\":\"request-42\"}}";
        let error = require_http_status(response, 201, "create workspace")
            .expect_err("unexpected status must fail");
        assert!(error.contains("not accessible"), "{error}");
        assert!(error.contains("request-42"), "{error}");
        assert!(
            require_http_status(b"HTTP/1.1 201 Created\r\n\r\n{}", 201, "create workspace").is_ok()
        );
    }

    #[test]
    fn sidecar_log_rotation_keeps_the_configured_file_count() {
        let directory = temporary_directory("logs");
        let mut writer = RotatingLogWriter::open(&directory, 32, 3).expect("open test log");
        for index in 0..8 {
            writer
                .write_line("stderr", &format!("line-{index}-with-content"))
                .expect("write test log");
        }
        drop(writer);

        assert!(directory.join("axklib-server.log").is_file());
        assert!(directory.join("axklib-server.log.1").is_file());
        assert!(directory.join("axklib-server.log.2").is_file());
        assert!(!directory.join("axklib-server.log.3").exists());
        std::fs::remove_dir_all(directory).expect("remove test logs");
    }
}
