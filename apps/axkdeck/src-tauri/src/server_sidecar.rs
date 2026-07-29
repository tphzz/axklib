use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::{ffi::OsString, fmt::Write as _};

use serde::{Deserialize, Serialize};
use url::{Host, Url};

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
    _runtime_directory: PrivateRuntimeDirectory,
    connection: FrontendConnection,
    shutdown_timeout: Duration,
}

struct PrivateRuntimeDirectory {
    path: PathBuf,
    #[cfg(unix)]
    handle: File,
}

impl PrivateRuntimeDirectory {
    fn create(parent: &Path) -> Result<Self, String> {
        for _ in 0..32 {
            let mut random = [0_u8; 16];
            getrandom::fill(&mut random)
                .map_err(|error| format!("generate sidecar runtime directory name: {error}"))?;
            let mut name = String::with_capacity(random.len() * 2);
            for byte in random {
                write!(&mut name, "{byte:02x}").expect("writing to a String cannot fail");
            }
            let path = parent.join(format!("axkdeck-server-{name}"));
            match create_private_directory(&path) {
                Ok(()) => return Self::open(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => {
                    return Err(format!("create private sidecar runtime directory: {error}"));
                }
            }
        }
        Err("could not reserve a unique sidecar runtime directory".to_owned())
    }

    fn open(path: PathBuf) -> Result<Self, String> {
        validate_private_directory(&path)?;
        #[cfg(unix)]
        let handle = {
            use rustix::fs::{Mode, OFlags, open};
            let descriptor = open(
                &path,
                OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|error| format!("retain sidecar runtime directory: {error}"))?;
            File::from(descriptor)
        };
        Ok(Self {
            path,
            #[cfg(unix)]
            handle,
        })
    }

    #[cfg(test)]
    fn path(&self) -> &Path {
        &self.path
    }

    fn connection_path(&self) -> PathBuf {
        self.path.join("connection.json")
    }

    fn read_connection(&self) -> Result<Vec<u8>, String> {
        #[cfg(unix)]
        {
            use rustix::fs::{Mode, OFlags, openat};
            let descriptor = openat(
                &self.handle,
                "connection.json",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|error| format!("read sidecar connection file: {error}"))?;
            let mut file = File::from(descriptor);
            let metadata = file
                .metadata()
                .map_err(|error| format!("inspect sidecar connection file: {error}"))?;
            if !metadata.is_file() {
                return Err("sidecar connection entry is not a regular file".to_owned());
            }
            let mut document = Vec::new();
            file.read_to_end(&mut document)
                .map_err(|error| format!("read sidecar connection file: {error}"))?;
            Ok(document)
        }
        #[cfg(windows)]
        {
            validate_private_directory(&self.path)?;
            let path = self.connection_path();
            let metadata = std::fs::symlink_metadata(&path)
                .map_err(|error| format!("inspect sidecar connection file: {error}"))?;
            if !metadata.is_file() {
                return Err("sidecar connection entry is not a regular file".to_owned());
            }
            std::fs::read(path).map_err(|error| format!("read sidecar connection file: {error}"))
        }
    }

    fn remove_connection(&self) -> Result<(), String> {
        #[cfg(unix)]
        {
            use rustix::fs::{AtFlags, unlinkat};
            unlinkat(&self.handle, "connection.json", AtFlags::empty())
                .map_err(|error| format!("remove consumed sidecar connection file: {error}"))
        }
        #[cfg(windows)]
        {
            validate_private_directory(&self.path)?;
            std::fs::remove_file(self.connection_path())
                .map_err(|error| format!("remove consumed sidecar connection file: {error}"))
        }
    }
}

impl Drop for PrivateRuntimeDirectory {
    fn drop(&mut self) {
        if validate_private_directory(&self.path).is_ok() {
            let _ = self.remove_connection();
            let _ = std::fs::remove_dir(&self.path);
        }
    }
}

#[cfg(unix)]
fn create_private_directory(path: &Path) -> std::io::Result<()> {
    use std::os::unix::fs::DirBuilderExt;
    let mut builder = std::fs::DirBuilder::new();
    builder.mode(0o700).create(path)
}

#[cfg(windows)]
fn create_private_directory(path: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use std::ptr::{null, null_mut};
    use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_ALL, LocalFree};
    use windows_sys::Win32::Security::Authorization::{
        EXPLICIT_ACCESS_W, SET_ACCESS, SetEntriesInAclW, TRUSTEE_IS_SID, TRUSTEE_IS_USER, TRUSTEE_W,
    };
    use windows_sys::Win32::Security::{
        GetTokenInformation, InitializeSecurityDescriptor, NO_INHERITANCE, SE_DACL_PROTECTED,
        SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR, SetSecurityDescriptorControl,
        SetSecurityDescriptorDacl, TOKEN_QUERY, TOKEN_USER, TokenUser,
    };
    use windows_sys::Win32::Storage::FileSystem::CreateDirectoryW;
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

    let mut token = null_mut();
    unsafe {
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    let result = (|| {
        let mut token_size = 0_u32;
        unsafe {
            let _ = GetTokenInformation(token, TokenUser, null_mut(), 0, &mut token_size);
        }
        if token_size == 0 {
            return Err(std::io::Error::last_os_error());
        }
        let word_size = std::mem::size_of::<usize>();
        let mut token_storage = vec![0_usize; (token_size as usize).div_ceil(word_size)];
        if unsafe {
            GetTokenInformation(
                token,
                TokenUser,
                token_storage.as_mut_ptr().cast(),
                token_size,
                &mut token_size,
            )
        } == 0
        {
            return Err(std::io::Error::last_os_error());
        }
        let token_user = unsafe { &*token_storage.as_ptr().cast::<TOKEN_USER>() };
        let access = EXPLICIT_ACCESS_W {
            grfAccessPermissions: GENERIC_ALL,
            grfAccessMode: SET_ACCESS,
            grfInheritance: NO_INHERITANCE,
            Trustee: TRUSTEE_W {
                pMultipleTrustee: null_mut(),
                MultipleTrusteeOperation: 0,
                TrusteeForm: TRUSTEE_IS_SID,
                TrusteeType: TRUSTEE_IS_USER,
                ptstrName: token_user.User.Sid.cast(),
            },
        };
        let mut acl = null_mut();
        let acl_status = unsafe { SetEntriesInAclW(1, &access, null(), &mut acl) };
        if acl_status != 0 {
            return Err(std::io::Error::from_raw_os_error(acl_status as i32));
        }
        let mut descriptor = SECURITY_DESCRIPTOR::default();
        let descriptor_ready = unsafe {
            InitializeSecurityDescriptor((&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(), 1)
                != 0
                && SetSecurityDescriptorDacl(
                    (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
                    1,
                    acl,
                    0,
                ) != 0
                && SetSecurityDescriptorControl(
                    (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
                    SE_DACL_PROTECTED,
                    SE_DACL_PROTECTED,
                ) != 0
        };
        if !descriptor_ready {
            unsafe {
                LocalFree(acl.cast());
            }
            return Err(std::io::Error::last_os_error());
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
            bInheritHandle: 0,
        };
        let wide = path
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect::<Vec<_>>();
        let created = unsafe { CreateDirectoryW(wide.as_ptr(), &attributes) };
        let error = if created == 0 {
            Some(std::io::Error::last_os_error())
        } else {
            None
        };
        unsafe {
            LocalFree(acl.cast());
        }
        match error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    })();
    unsafe {
        CloseHandle(token);
    }
    result
}

fn validate_private_directory(path: &Path) -> Result<(), String> {
    let metadata = std::fs::symlink_metadata(path)
        .map_err(|error| format!("inspect sidecar runtime directory: {error}"))?;
    if !metadata.is_dir() {
        return Err("sidecar runtime path is not a directory".to_owned());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if metadata.uid() != rustix::process::geteuid().as_raw() {
            return Err("sidecar runtime directory is not owned by the current user".to_owned());
        }
        if metadata.permissions().mode() & 0o077 != 0 {
            return Err("sidecar runtime directory permissions are not owner-only".to_owned());
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_ATTRIBUTE_REPARSE_POINT;
        if metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err("sidecar runtime directory must not be a reparse point".to_owned());
        }
    }
    Ok(())
}

impl ServerSidecar {
    pub fn launch_if_available(
        log_directory: &Path,
        state_directory: &Path,
    ) -> Result<Option<Self>, String> {
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
        Self::launch(&binary, log_directory, state_directory).map(Some)
    }

    fn launch(binary: &Path, log_directory: &Path, state_directory: &Path) -> Result<Self, String> {
        prepare_persistent_state_directory(state_directory)?;
        let runtime_directory = PrivateRuntimeDirectory::create(&std::env::temp_dir())?;
        let connection_path = runtime_directory.connection_path();
        let arguments = sidecar_arguments(state_directory, &connection_path);
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
fn request_shutdown(connection: &FrontendConnection) -> Result<(), String> {
    let parsed = parse_loopback_endpoint(&connection.base_url, "http", "/api/v1")?;
    let authority = format!(
        "127.0.0.1:{}",
        parsed
            .port()
            .ok_or_else(|| "sidecar base URL requires an explicit port".to_owned())?
    );
    let mut stream = TcpStream::connect(&authority)
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
mod tests {
    use super::{
        ConnectionFile, FrontendConnection, PrivateRuntimeDirectory, RotatingLogWriter,
        ServerSidecar, prepare_persistent_state_directory, request_shutdown, require_http_status,
        sidecar_arguments, validate_connection, wait_for_connection,
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

        for invalid in [
            "http://127.0.0.1.evil.example:7300/api/v1",
            "http://user@127.0.0.1:7300/api/v1",
            "http://127.0.0.1:7300/api/v1/extra",
            "http://127.0.0.1:7300/api/v1?redirect=evil",
            "http://localhost:7300/api/v1",
            "http://127.0.0.1/api/v1",
        ] {
            let mut malformed = connection(42);
            malformed.base_url = invalid.to_owned();
            assert!(
                validate_connection(&malformed, 42, "0.0.0", "main-1234567").is_err(),
                "{invalid}"
            );
        }
        let mut mismatched_websocket = connection(42);
        mismatched_websocket.websocket_url = "ws://127.0.0.1:7301/api/v1/events".to_owned();
        assert!(validate_connection(&mismatched_websocket, 42, "0.0.0", "main-1234567").is_err());

        let connection = connection(42);
        assert!(validate_connection(&connection, 42, "1.0.0", "main-1234567").is_err());
        assert!(validate_connection(&connection, 42, "0.0.0", "other-7654321").is_err());
    }

    #[test]
    fn runtime_directories_are_private_and_unpredictable() {
        let parent = temporary_directory("runtime-parent");
        std::fs::create_dir(&parent).expect("create runtime parent");
        let first = PrivateRuntimeDirectory::create(&parent).expect("create first runtime");
        let second = PrivateRuntimeDirectory::create(&parent).expect("create second runtime");

        assert_ne!(first.path(), second.path());
        assert!(first.path().is_dir());
        assert!(second.path().is_dir());
        assert!(
            !first
                .path()
                .file_name()
                .expect("runtime directory name")
                .to_string_lossy()
                .contains(&std::process::id().to_string())
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(first.path())
                    .expect("inspect runtime directory")
                    .permissions()
                    .mode()
                    & 0o777,
                0o700
            );
        }
        drop(first);
        drop(second);
        std::fs::remove_dir(parent).expect("remove runtime parent");
    }

    #[cfg(unix)]
    #[test]
    fn runtime_directory_rejects_symlinks_and_broad_permissions() {
        use std::os::unix::fs::{PermissionsExt, symlink};

        let parent = temporary_directory("unsafe-runtime");
        let target = parent.join("target");
        let link = parent.join("link");
        std::fs::create_dir_all(&target).expect("create target");
        symlink(&target, &link).expect("create runtime symlink");
        assert!(PrivateRuntimeDirectory::open(link).is_err());

        std::fs::set_permissions(&target, std::fs::Permissions::from_mode(0o755))
            .expect("widen target permissions");
        assert!(PrivateRuntimeDirectory::open(target.clone()).is_err());
        std::fs::remove_file(parent.join("link")).expect("remove link");
        std::fs::remove_dir_all(parent).expect("remove unsafe runtime tree");
    }

    #[test]
    fn persistent_state_is_secured_without_removing_recovery_data() {
        let directory = temporary_directory("persistent");
        std::fs::create_dir_all(&directory).expect("create persistent state");
        std::fs::write(directory.join("pending.axkjournal"), b"recovery")
            .expect("write recovery state");

        prepare_persistent_state_directory(&directory).expect("prepare persistent state");

        assert_eq!(
            std::fs::read(directory.join("pending.axkjournal")).expect("read recovery state"),
            b"recovery"
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&directory)
                    .expect("inspect state directory")
                    .permissions()
                    .mode()
                    & 0o777,
                0o700
            );
        }
        std::fs::remove_dir_all(directory).expect("remove test state");
    }

    #[test]
    fn readiness_reports_timeout_and_child_exit_separately() {
        let directory = temporary_directory("readiness");
        let parent = directory.join("parent");
        std::fs::create_dir_all(&parent).expect("create readiness parent");
        let runtime = PrivateRuntimeDirectory::create(&parent).expect("create readiness runtime");

        let mut waiting = waiting_child();
        let timeout = wait_for_connection(&runtime, &mut waiting, Duration::from_millis(30))
            .expect_err("missing connection file must time out");
        assert!(timeout.contains("within 30 ms"), "{timeout}");
        waiting.kill().expect("kill waiting child");
        waiting.wait().expect("reap waiting child");

        let mut exiting = exiting_child();
        let exited = wait_for_connection(&runtime, &mut exiting, Duration::from_secs(1))
            .expect_err("exited child must be reported");
        assert!(exited.contains("exited before readiness"), "{exited}");
        exiting.wait().expect("reap exiting child");
        drop(runtime);
        std::fs::remove_dir_all(directory).expect("remove readiness state");
    }

    #[test]
    fn crashed_sidecar_is_reported_and_drop_reaps_the_child_and_state() {
        let directory = temporary_directory("crash");
        let parent = directory.join("parent");
        std::fs::create_dir_all(&parent).expect("create crash parent");
        let runtime = PrivateRuntimeDirectory::create(&parent).expect("create crash runtime");
        let mut child = exiting_child();
        let pid = child.id();
        child.wait().expect("wait for child crash");
        let sidecar = ServerSidecar {
            child: Mutex::new(Some(child)),
            log_threads: Vec::new(),
            _runtime_directory: runtime,
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
        assert!(
            std::fs::read_dir(&parent)
                .expect("inspect crash parent")
                .next()
                .is_none()
        );
        std::fs::remove_dir_all(directory).expect("remove crash parent");
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
