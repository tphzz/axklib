use serde::Serialize;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, State, WebviewWindow};

use crate::diagnostic_level::Level;
use crate::remote_settings::ServerConnectionState;
use crate::server_sidecar::FrontendConnection;

static CAPTURE: AtomicU8 = AtomicU8::new(3);

pub fn retained_enabled(level: log::Level) -> bool {
    level as u8 <= CAPTURE.load(Ordering::Relaxed)
}
pub fn retained_filter() -> log::LevelFilter {
    match CAPTURE.load(Ordering::Relaxed) {
        1 => log::LevelFilter::Error,
        2 => log::LevelFilter::Warn,
        5 => log::LevelFilter::Trace,
        4 => log::LevelFilter::Debug,
        _ => log::LevelFilter::Info,
    }
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CaptureState {
    pub level: Level,
    pub local_server: String,
    pub error: Option<String>,
}

struct Control {
    open: bool,
    generation: u64,
    state: CaptureState,
}
struct Applied {
    generation: u64,
    endpoint: String,
    renewed: Instant,
}
struct Inner {
    control: Mutex<Control>,
    applied: Mutex<Applied>,
    stopped: AtomicBool,
    worker: Mutex<Option<std::thread::JoinHandle<()>>>,
}

#[derive(Clone)]
pub struct CaptureRuntime(Arc<Inner>);

impl Default for CaptureRuntime {
    fn default() -> Self {
        Self(Arc::new(Inner {
            control: Mutex::new(Control {
                open: false,
                generation: 0,
                state: CaptureState {
                    level: Level::Info,
                    local_server: "unavailable".into(),
                    error: None,
                },
            }),
            applied: Mutex::new(Applied {
                generation: u64::MAX,
                endpoint: String::new(),
                renewed: Instant::now(),
            }),
            stopped: AtomicBool::new(false),
            worker: Mutex::new(None),
        }))
    }
}

impl CaptureRuntime {
    pub fn opened(&self) {
        self.0
            .control
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .open = true;
    }
    pub fn close(&self, app: &AppHandle) {
        let mut control = self
            .0
            .control
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        control.open = false;
        control.generation += 1;
        control.state.level = Level::Info;
        control.state.error = None;
        CAPTURE.store(3, Ordering::Relaxed);
        drop(control);
        self.broadcast(app);
    }
    pub fn stop(&self) {
        self.0.stopped.store(true, Ordering::Relaxed);
        if let Some(worker) = self
            .0
            .worker
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take()
        {
            let _ = worker.join();
        }
    }
    fn snapshot(&self) -> CaptureState {
        self.0
            .control
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .state
            .clone()
    }
    fn broadcast(&self, app: &AppHandle) {
        let _ = app.emit("diagnostic-capture-changed", self.snapshot());
    }
    pub fn start(&self, app: AppHandle) -> Result<(), String> {
        let runtime = self.clone();
        let worker = std::thread::Builder::new()
            .name("diagnostic-capture".into())
            .spawn(move || {
                while !runtime.0.stopped.load(Ordering::Relaxed) {
                    if let Err(error) = runtime.synchronize(&app, None) {
                        let mut control = runtime
                            .0
                            .control
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        control.state.error = Some(error);
                        drop(control);
                        runtime.broadcast(&app);
                    }
                    std::thread::sleep(Duration::from_secs(1));
                }
            })
            .map_err(|error| format!("start diagnostic capture control: {error}"))?;
        *self
            .0
            .worker
            .lock()
            .map_err(|_| "Capture worker is unavailable")? = Some(worker);
        Ok(())
    }
    fn synchronize(
        &self,
        app: &AppHandle,
        requested: Option<Level>,
    ) -> Result<CaptureState, String> {
        let mut applied = self
            .0
            .applied
            .lock()
            .map_err(|_| "Capture control is unavailable")?;
        let (generation, target, open) = {
            let control = self
                .0
                .control
                .lock()
                .map_err(|_| "Capture state is unavailable")?;
            (
                control.generation,
                requested.unwrap_or(control.state.level),
                control.open,
            )
        };
        if requested.is_some() && !open {
            return Err("The Logs window is closed".into());
        }
        let connection = app
            .try_state::<ServerConnectionState>()
            .map(|state| state.try_connection())
            .transpose()?
            .flatten();
        let local = connection
            .as_ref()
            .filter(|connection| connection.mode == "local");
        let endpoint = local.map_or("", |connection| connection.base_url.as_str());
        if let Some(connection) = local {
            if requested.is_some()
                || applied.generation != generation
                || applied.endpoint != endpoint
                || (target != Level::Info && applied.renewed.elapsed() >= Duration::from_secs(10))
            {
                request_capture(connection, target)?;
                applied.generation = generation;
                applied.endpoint = endpoint.to_owned();
                applied.renewed = Instant::now();
            }
        }
        let mut control = self
            .0
            .control
            .lock()
            .map_err(|_| "Capture state is unavailable")?;
        if control.generation != generation {
            return Err("Capture changed while the request was pending".into());
        }
        let previous = (
            control.state.level,
            control.state.local_server.clone(),
            control.state.error.clone(),
        );
        control.state.level = target;
        control.state.local_server = if local.is_some() {
            "ready"
        } else if connection.is_some() {
            "remote"
        } else {
            "unavailable"
        }
        .into();
        control.state.error = None;
        CAPTURE.store(target.filter() as u8, Ordering::Relaxed);
        let changed = previous
            != (
                control.state.level,
                control.state.local_server.clone(),
                control.state.error.clone(),
            );
        let state = control.state.clone();
        drop(control);
        if changed {
            self.broadcast(app);
        }
        Ok(state)
    }
}

fn request_capture(connection: &FrontendConnection, level: Level) -> Result<(), String> {
    if connection.mode != "local" {
        return Err("Remote capture is not supported".into());
    }
    let url = url::Url::parse(&connection.base_url).map_err(|error| error.to_string())?;
    if url.scheme() != "http"
        || url.host_str() != Some("127.0.0.1")
        || url.port().is_none()
        || url.path() != "/api/v1"
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err("Capture requires a canonical local sidecar connection".into());
    }
    let _ = rustls::crypto::ring::default_provider().install_default();
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(2))
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .map_err(|error| error.to_string())?;
    let response = client
        .put(format!("{}/diagnostics/capture", connection.base_url))
        .bearer_auth(&connection.bearer_token)
        .header("Content-Type", "application/json")
        .body(format!(
            "{{\"level\":\"{}\"}}",
            level.name().to_ascii_uppercase()
        ))
        .send()
        .map_err(|error| format!("Update local server capture: {error}"))?;
    if !response.status().is_success() {
        return Err(format!(
            "Local server rejected capture: HTTP {}",
            response.status()
        ));
    }
    let bytes = std::io::Read::bytes(response)
        .take(8193)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| error.to_string())?;
    if bytes.len() > 8192 {
        return Err("Capture response exceeds its limit".into());
    }
    let body: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
    if body
        .get("data")
        .and_then(|value| value.get("level"))
        .and_then(serde_json::Value::as_str)
        != Some(level.name().to_ascii_uppercase().as_str())
    {
        return Err("Local server did not acknowledge the capture level".into());
    }
    Ok(())
}

#[tauri::command]
pub fn diagnostic_capture_state(state: State<'_, CaptureRuntime>) -> CaptureState {
    state.snapshot()
}

#[tauri::command]
pub async fn set_diagnostic_capture(
    app: AppHandle,
    window: WebviewWindow,
    state: State<'_, CaptureRuntime>,
    level: Level,
) -> Result<CaptureState, String> {
    if window.label() != crate::diagnostic_logs::WINDOW_LABEL {
        return Err("Capture can only be changed in the Logs window".into());
    }
    let runtime = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || runtime.synchronize(&app, Some(level)))
        .await
        .map_err(|error| error.to_string())?
}

#[tauri::command]
pub fn diagnostic_log_level() -> &'static str {
    crate::log_level_name(std::cmp::max(
        crate::configured_log_level(),
        retained_filter(),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};

    fn configure_capture_stream(stream: &TcpStream) {
        // Windows accepts sockets in the listener's nonblocking mode.
        stream.set_nonblocking(false).unwrap();
        let timeout = Some(Duration::from_secs(3));
        stream.set_read_timeout(timeout).unwrap();
        stream.set_write_timeout(timeout).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn capture_stream_resets_inherited_nonblocking_mode() {
        use rustix::fs::{OFlags, fcntl_getfl};

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let _client =
            TcpStream::connect_timeout(&listener.local_addr().unwrap(), Duration::from_secs(3))
                .unwrap();
        let (stream, _) = listener.accept().unwrap();
        stream.set_nonblocking(true).unwrap();
        assert!(fcntl_getfl(&stream).unwrap().contains(OFlags::NONBLOCK));

        configure_capture_stream(&stream);

        assert!(!fcntl_getfl(&stream).unwrap().contains(OFlags::NONBLOCK));
        assert_eq!(stream.read_timeout().unwrap(), Some(Duration::from_secs(3)));
        assert_eq!(
            stream.write_timeout().unwrap(),
            Some(Duration::from_secs(3))
        );
    }

    fn capture_reply(body: &str, level: Level) -> (Result<(), String>, String) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let port = listener.local_addr().unwrap().port();
        let body = body.to_owned();
        let worker = std::thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(3);
            let (mut socket, _) = loop {
                match listener.accept() {
                    Ok(socket) => break socket,
                    Err(error)
                        if error.kind() == std::io::ErrorKind::WouldBlock
                            && Instant::now() < deadline =>
                    {
                        std::thread::sleep(Duration::from_millis(10))
                    }
                    Err(error) => panic!("capture listener: {error}"),
                }
            };
            configure_capture_stream(&socket);
            let mut request = Vec::new();
            let mut buffer = [0_u8; 1024];
            loop {
                let count = socket.read(&mut buffer).unwrap();
                assert!(count > 0);
                request.extend_from_slice(&buffer[..count]);
                if let Some(end) = request.windows(4).position(|value| value == b"\r\n\r\n") {
                    let header = String::from_utf8_lossy(&request[..end]);
                    let length = header
                        .lines()
                        .find_map(|line| {
                            line.to_ascii_lowercase()
                                .strip_prefix("content-length: ")
                                .and_then(|value| value.parse::<usize>().ok())
                        })
                        .unwrap();
                    if request.len() >= end + 4 + length {
                        break;
                    }
                }
            }
            write!(
                socket,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                body.len()
            )
            .unwrap();
            String::from_utf8(request).unwrap()
        });
        let result = request_capture(
            &FrontendConnection {
                base_url: format!("http://127.0.0.1:{port}/api/v1"),
                bearer_token: "capture-test-token".into(),
                mode: "local".into(),
            },
            level,
        );
        (result, worker.join().unwrap())
    }

    #[test]
    fn capture_requires_authenticated_acknowledgement_of_the_requested_level() {
        for level in [
            Level::Error,
            Level::Warning,
            Level::Info,
            Level::Debug,
            Level::Trace,
        ] {
            let name = level.name().to_ascii_uppercase();
            let (result, request) =
                capture_reply(&format!(r#"{{"data":{{"level":"{name}"}}}}"#), level);
            assert!(result.is_ok(), "{result:?}");
            assert!(request.starts_with("PUT /api/v1/diagnostics/capture HTTP/1.1\r\n"));
            assert!(request.contains("authorization: Bearer capture-test-token"));
            assert!(request.ends_with(&format!(r#"{{"level":"{name}"}}"#)));
        }
        assert!(
            capture_reply(r#"{"data":{"level":"INFO"}}"#, Level::Debug)
                .0
                .is_err()
        );
    }

    #[test]
    fn capture_never_contacts_remote_or_noncanonical_urls() {
        for (mode, base_url) in [
            ("remote", "http://127.0.0.1:1/api/v1"),
            ("local", "http://localhost:1/api/v1"),
            ("local", "http://127.0.0.1:1/api/v1?token=secret"),
        ] {
            let error = request_capture(
                &FrontendConnection {
                    mode: mode.into(),
                    base_url: base_url.into(),
                    bearer_token: "test".into(),
                },
                Level::Info,
            )
            .unwrap_err();
            assert!(
                error == "Remote capture is not supported"
                    || error == "Capture requires a canonical local sidecar connection"
            );
        }
    }
    #[test]
    fn capture_only_accepts_the_five_runtime_levels() {
        assert_eq!(
            serde_json::from_str::<Level>("\"info\"").unwrap(),
            Level::Info
        );
        for value in ["off", "warn", "unclassified", "DEBUG"] {
            assert!(serde_json::from_str::<Level>(&format!("\"{value}\"")).is_err());
        }
        assert_eq!(Level::Debug.filter(), log::LevelFilter::Debug);
        assert_eq!(
            serde_json::from_str::<Level>("\"error\"").unwrap(),
            Level::Error
        );
        assert_eq!(
            serde_json::from_str::<Level>("\"warning\"").unwrap(),
            Level::Warning
        );
    }
}
