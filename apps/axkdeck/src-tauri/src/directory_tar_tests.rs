use crate::directory_tar::{ExportControl, extract_directory_tar, receive_archive};
use std::io::{Cursor, Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

fn root() -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let root = std::env::temp_dir().join(format!(
        "axkdeck-tar-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir(&root).unwrap();
    root
}

fn archive(entries: &[(&str, u8, &[u8])]) -> Vec<u8> {
    let mut bytes = Vec::new();
    for (name, kind, payload) in entries {
        let mut header = [0u8; 512];
        header[..name.len()].copy_from_slice(name.as_bytes());
        header[124..136].copy_from_slice(format!("{:011o}\0", payload.len()).as_bytes());
        header[148..156].fill(b' ');
        header[156] = *kind;
        header[257..263].copy_from_slice(b"ustar\0");
        let checksum: u64 = header.iter().map(|byte| u64::from(*byte)).sum();
        header[148..156].copy_from_slice(format!("{checksum:06o}\0 ").as_bytes());
        bytes.extend(header);
        bytes.extend_from_slice(payload);
        bytes.resize(bytes.len().div_ceil(512) * 512, 0);
    }
    bytes.extend([0u8; 1024]);
    bytes
}

#[test]
fn bounded_staging_preserves_names_empty_entries_and_payloads() {
    let root = root();
    let output = root.join("out");
    let mut tar = Cursor::new(archive(&[
        ("Folder #1/", b'5', &[]),
        ("Folder #1/Empty", b'0', &[]),
        ("Folder #1/RAW%", b'0', &[0, 255, 42]),
    ]));
    extract_directory_tar(&mut tar, &output, &ExportControl::default()).unwrap();
    assert_eq!(
        std::fs::read(output.join("Folder #1/RAW%")).unwrap(),
        [0, 255, 42]
    );
    assert_eq!(
        std::fs::metadata(output.join("Folder #1/Empty"))
            .unwrap()
            .len(),
        0
    );
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn a_destination_created_during_extraction_is_preserved_and_staging_is_removed() {
    struct DestinationCreator {
        input: Cursor<Vec<u8>>,
        destination: PathBuf,
    }
    impl Read for DestinationCreator {
        fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
            let count = self.input.read(buffer)?;
            if count == 0 {
                std::fs::create_dir(&self.destination)?;
            }
            Ok(count)
        }
    }
    impl Seek for DestinationCreator {
        fn seek(&mut self, position: SeekFrom) -> std::io::Result<u64> {
            self.input.seek(position)
        }
    }
    let root = root();
    let output = root.join("out");
    let mut tar = DestinationCreator {
        input: Cursor::new(archive(&[("payload", b'0', b"new")])),
        destination: output.clone(),
    };
    let error = extract_directory_tar(&mut tar, &output, &ExportControl::default()).unwrap_err();
    assert!(error.contains("publish export folder"), "{error}");
    assert!(output.is_dir());
    assert_eq!(std::fs::read_dir(&output).unwrap().count(), 0);
    assert_eq!(std::fs::read_dir(&root).unwrap().count(), 1);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn resource_bounds_fail_without_publishing_or_leaking_staging() {
    for control in [
        ExportControl {
            maximum_archive_bytes: 1024,
            ..ExportControl::default()
        },
        ExportControl {
            maximum_payload_bytes: 2,
            ..ExportControl::default()
        },
        ExportControl {
            maximum_entries: 1,
            ..ExportControl::default()
        },
    ] {
        let root = root();
        let mut tar = Cursor::new(archive(&[("FIRST", b'0', &[1, 2]), ("SECOND", b'0', &[3])]));
        assert!(extract_directory_tar(&mut tar, &root.join("out"), &control).is_err());
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        std::fs::remove_dir(root).unwrap();
    }
}

#[test]
fn unsafe_duplicate_link_and_trailing_entries_are_not_partial_successes() {
    for mut bytes in [
        archive(&[("../escape", b'0', &[])]),
        archive(&[(r"A\..\escape", b'0', &[])]),
        archive(&[("A:stream", b'0', &[])]),
        archive(&[("LINK", b'2', &[])]),
        archive(&[("DIR/", b'5', &[]), ("DIR/", b'5', &[])]),
        {
            let mut bytes = archive(&[("A", b'0', &[])]);
            bytes.extend(archive(&[("HIDDEN", b'0', &[])]));
            bytes
        },
    ] {
        let root = root();
        assert!(
            extract_directory_tar(
                &mut Cursor::new(&mut bytes),
                &root.join("out"),
                &ExportControl::default()
            )
            .is_err()
        );
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        assert!(!root.join("escape").exists());
        std::fs::remove_dir(root).unwrap();
    }
}

struct CancellingReader<'a> {
    inner: Cursor<Vec<u8>>,
    control: &'a ExportControl,
    at: u64,
}
impl Read for CancellingReader<'_> {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        let count = self.inner.read(buffer)?;
        if self.inner.position() >= self.at {
            self.control.cancelled.store(true, Ordering::Relaxed);
        }
        Ok(count)
    }
}
impl Seek for CancellingReader<'_> {
    fn seek(&mut self, position: SeekFrom) -> std::io::Result<u64> {
        self.inner.seek(position)
    }
}

#[test]
fn cancellation_during_payload_or_after_last_header_removes_staging() {
    let bytes = archive(&[("A", b'0', &vec![7u8; 2 * 1024 * 1024])]);
    for at in [0, 1024, bytes.len() as u64] {
        let root = root();
        let control = ExportControl::default();
        if at == 0 {
            control.cancelled.store(true, Ordering::Relaxed);
        }
        let mut tar = CancellingReader {
            inner: Cursor::new(bytes.clone()),
            control: &control,
            at,
        };
        assert!(
            extract_directory_tar(&mut tar, &root.join("out"), &control)
                .unwrap_err()
                .contains("cancelled")
        );
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        std::fs::remove_dir(root).unwrap();
    }
}

#[test]
fn receiving_archive_checks_size_before_io_and_preserves_exact_bytes() {
    let control = ExportControl {
        maximum_archive_bytes: 3,
        ..ExportControl::default()
    };
    let mut input = Cursor::new([1, 2, 3]);
    let mut output = Vec::new();
    assert!(receive_archive(&mut input, &mut output, 4, &control).is_err());
    assert_eq!(input.position(), 0);
    assert!(output.is_empty());
    receive_archive(&mut input, &mut output, 3, &control).unwrap();
    assert_eq!(output, [1, 2, 3]);
    for expected in [2, 4] {
        assert!(
            receive_archive(
                &mut Cursor::new([1, 2, 3]),
                &mut Vec::new(),
                expected,
                &ExportControl::default()
            )
            .is_err()
        );
    }
}

#[test]
fn receiving_archive_cancels_before_writing_a_just_received_chunk() {
    let control = ExportControl::default();
    let mut input = CancellingReader {
        inner: Cursor::new(vec![7; 1024]),
        control: &control,
        at: 1,
    };
    let mut output = Vec::new();
    assert!(
        receive_archive(&mut input, &mut output, 1024, &control)
            .unwrap_err()
            .contains("cancelled")
    );
    assert!(output.is_empty());
}

#[test]
fn receiving_archive_propagates_read_and_write_failures() {
    struct Broken;
    impl Read for Broken {
        fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
            Err(std::io::Error::other("injected read"))
        }
    }
    impl std::io::Write for Broken {
        fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
            Err(std::io::Error::other("injected write"))
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    assert!(
        receive_archive(&mut Broken, &mut Vec::new(), 1, &ExportControl::default())
            .unwrap_err()
            .contains("injected read")
    );
    assert!(
        receive_archive(
            &mut Cursor::new([1]),
            &mut Broken,
            1,
            &ExportControl::default()
        )
        .unwrap_err()
        .contains("injected write")
    );
}

fn configure_test_http_stream(stream: &std::net::TcpStream) {
    // Accepted sockets can inherit the listener's nonblocking mode on BSD systems.
    stream.set_nonblocking(false).unwrap();
    let timeout = Some(std::time::Duration::from_secs(5));
    stream.set_read_timeout(timeout).unwrap();
    stream.set_write_timeout(timeout).unwrap();
}

#[cfg(unix)]
#[test]
fn test_http_stream_resets_inherited_nonblocking_mode() {
    use rustix::fs::{OFlags, fcntl_getfl};
    use std::net::{TcpListener, TcpStream};
    use std::time::Duration;

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let _client =
        TcpStream::connect_timeout(&listener.local_addr().unwrap(), Duration::from_secs(5))
            .unwrap();
    let (stream, _) = listener.accept().unwrap();
    stream.set_nonblocking(true).unwrap();
    assert!(fcntl_getfl(&stream).unwrap().contains(OFlags::NONBLOCK));

    configure_test_http_stream(&stream);

    assert!(!fcntl_getfl(&stream).unwrap().contains(OFlags::NONBLOCK));
    assert_eq!(stream.read_timeout().unwrap(), Some(Duration::from_secs(5)));
    assert_eq!(
        stream.write_timeout().unwrap(),
        Some(Duration::from_secs(5))
    );
}

#[test]
fn retained_http_download_publishes_only_complete_valid_archives_and_cleans_staging() {
    use crate::local_directory_exports::download_retained_directory_export;
    use crate::server_sidecar::FrontendConnection;
    use std::io::Write;
    use std::net::TcpListener;
    use std::time::{Duration, Instant};

    let valid = archive(&[("Folder/", b'5', &[]), ("Folder/RAW", b'0', &[1, 2, 3])]);
    for (body, expected, success) in [
        (valid.clone(), valid.len() as u64, true),
        (archive(&[("../escape", b'0', &[1])]), 2048, false),
        (vec![0; 512], 1024, false),
    ] {
        let root = root();
        let output = root.join("out");
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let base_url = format!("http://{}", listener.local_addr().unwrap());
        let server = std::thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(5);
            let mut stream = loop {
                match listener.accept() {
                    Ok((stream, _)) => break stream,
                    Err(error)
                        if error.kind() == std::io::ErrorKind::WouldBlock
                            && Instant::now() < deadline =>
                    {
                        std::thread::sleep(Duration::from_millis(5))
                    }
                    Err(error) => panic!("test HTTP accept: {error}"),
                }
            };
            configure_test_http_stream(&stream);
            let mut request = Vec::new();
            let mut byte = [0];
            while !request.ends_with(b"\r\n\r\n") {
                stream.read_exact(&mut byte).unwrap();
                request.push(byte[0]);
                assert!(request.len() < 8192);
            }
            let request = String::from_utf8(request).unwrap().to_ascii_lowercase();
            assert!(request.contains("get /api/v1/download-archives/test/content http/1.1"));
            assert!(request.contains("authorization: bearer test-token"));
            write!(
                stream,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            )
            .unwrap();
            stream.write_all(&body).unwrap();
        });
        let result = download_retained_directory_export(
            FrontendConnection {
                base_url,
                bearer_token: "test-token".to_owned(),
                mode: "remote".to_owned(),
            },
            output.clone(),
            "/api/v1/download-archives/test/content".to_owned(),
            expected,
            &ExportControl::default(),
        );
        server.join().unwrap();
        assert_eq!(result.is_ok(), success, "{result:?}");
        if success {
            assert_eq!(std::fs::read(output.join("Folder/RAW")).unwrap(), [1, 2, 3]);
            assert_eq!(std::fs::read_dir(&root).unwrap().count(), 1);
        } else {
            assert_eq!(std::fs::read_dir(&root).unwrap().count(), 0);
        }
        std::fs::remove_dir_all(root).unwrap();
    }
}
