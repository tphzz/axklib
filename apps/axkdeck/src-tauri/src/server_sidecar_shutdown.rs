use super::{FrontendConnection, parse_loopback_endpoint};
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use std::time::Duration;

// The parent-only runtime directory is already protected on every platform.
// The native parent monitor consumes this token-bound signal and drains jobs.
pub(super) fn signal_shutdown(path: &Path, connection: &FrontendConnection) -> Result<(), String> {
    let path = path.with_extension("shutdown");
    if std::fs::symlink_metadata(&path).is_ok_and(|metadata| metadata.is_file())
        && std::fs::read(&path).is_ok_and(|bytes| bytes == connection.bearer_token.as_bytes())
    {
        return Ok(());
    }
    let pending = path.with_extension("shutdown.pending");
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&pending)
        .map_err(|error| format!("create private shutdown signal: {error}"))?;
    let written = file
        .write_all(connection.bearer_token.as_bytes())
        .and_then(|()| file.sync_all());
    drop(file);
    if let Err(error) = written {
        let _ = std::fs::remove_file(&pending);
        return Err(format!("write private shutdown signal: {error}"));
    }
    if let Err(error) = std::fs::rename(&pending, &path) {
        let _ = std::fs::remove_file(&pending);
        return Err(format!("publish private shutdown signal: {error}"));
    }
    Ok(())
}

pub(super) fn request_shutdown(connection: &FrontendConnection) -> Result<(), String> {
    let parsed = parse_loopback_endpoint(&connection.base_url, "http", "/api/v1")?;
    let authority = format!(
        "127.0.0.1:{}",
        parsed
            .port()
            .ok_or("sidecar base URL requires an explicit port")?
    );
    let mut stream = TcpStream::connect(&authority)
        .map_err(|error| format!("connect for sidecar shutdown: {error}"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .map_err(|error| format!("set shutdown read timeout: {error}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(2)))
        .map_err(|error| format!("set shutdown write timeout: {error}"))?;
    let request = format!(
        "POST /api/v1/system/shutdown HTTP/1.1\r\nHost: {authority}\r\nAuthorization: Bearer {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        connection.bearer_token
    );
    stream
        .write_all(request.as_bytes())
        .map_err(|error| format!("write sidecar shutdown request: {error}"))?;
    let mut response = Vec::new();
    stream
        .take(4096)
        .read_to_end(&mut response)
        .map_err(|error| format!("read sidecar shutdown response: {error}"))?;
    if !response.starts_with(b"HTTP/1.1 202 ") {
        return Err("axklib-server rejected the sidecar shutdown request".to_owned());
    }
    Ok(())
}
