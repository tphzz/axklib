use super::{
    ConnectionFile, FrontendConnection, PrivateRuntimeDirectory, RotatingLogWriter, ServerSidecar,
    prepare_persistent_state_directory, request_shutdown, require_http_status, sidecar_arguments,
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
            .write_all(b"HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
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
