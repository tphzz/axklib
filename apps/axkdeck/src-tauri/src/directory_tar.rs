use crate::local_packages::candidate_id;
use std::collections::HashSet;
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

#[derive(Clone)]
pub(crate) struct ExportControl {
    pub cancelled: Arc<AtomicBool>,
    pub maximum_archive_bytes: u64,
    pub maximum_payload_bytes: u64,
    pub maximum_entries: usize,
}

impl Default for ExportControl {
    fn default() -> Self {
        Self {
            cancelled: Arc::new(AtomicBool::new(false)),
            maximum_archive_bytes: 4 * 1024 * 1024 * 1024,
            maximum_payload_bytes: 4 * 1024 * 1024 * 1024,
            maximum_entries: 100_000,
        }
    }
}

impl ExportControl {
    pub fn check(&self) -> Result<(), String> {
        if self.cancelled.load(Ordering::Relaxed) {
            Err("Directory export cancelled".to_owned())
        } else {
            Ok(())
        }
    }
}

pub(crate) fn receive_archive(
    input: &mut impl Read,
    output: &mut impl Write,
    expected_size: u64,
    control: &ExportControl,
) -> Result<(), String> {
    control.check()?;
    if expected_size > control.maximum_archive_bytes {
        return Err("Directory export exceeds the local save limit".to_owned());
    }
    let mut buffer = vec![0_u8; 1024 * 1024];
    let mut written = 0_u64;
    loop {
        control.check()?;
        let count = input
            .read(&mut buffer)
            .map_err(|error| format!("Read directory export download: {error}"))?;
        control.check()?;
        if count == 0 {
            break;
        }
        written = written
            .checked_add(count as u64)
            .ok_or_else(|| "Directory export download size overflow".to_owned())?;
        if written > expected_size {
            return Err("Directory export download exceeded its declared size".to_owned());
        }
        output
            .write_all(&buffer[..count])
            .map_err(|error| format!("Write directory export download: {error}"))?;
    }
    if written != expected_size {
        return Err("Directory export download ended before its declared size".to_owned());
    }
    Ok(())
}

fn parse_tar_octal(field: &[u8]) -> Result<u64, String> {
    let text = std::str::from_utf8(field)
        .map_err(|_| "export archive contains a non-ASCII numeric field".to_owned())?
        .trim_matches(['\0', ' ']);
    if text.is_empty() {
        return Ok(0);
    }
    u64::from_str_radix(text, 8)
        .map_err(|_| "export archive contains an invalid numeric field".to_owned())
}

pub(crate) fn checked_tar_path(header: &[u8; 512]) -> Result<PathBuf, String> {
    let field = |range: std::ops::Range<usize>| {
        let bytes = &header[range];
        &bytes[..bytes
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(bytes.len())]
    };
    let name = std::str::from_utf8(field(0..100))
        .map_err(|_| "export archive path is not valid UTF-8".to_owned())?;
    let prefix = std::str::from_utf8(field(345..500))
        .map_err(|_| "export archive path is not valid UTF-8".to_owned())?;
    let value = if prefix.is_empty() {
        name.to_owned()
    } else {
        format!("{prefix}/{name}")
    };
    if value.contains(['\\', ':'])
        || value
            .trim_end_matches('/')
            .split('/')
            .any(|part| part.is_empty() || part == "." || part == "..")
    {
        return Err("export archive contains an unsafe path".to_owned());
    }
    let path = PathBuf::from(value);
    if path.as_os_str().is_empty()
        || path.is_absolute()
        || path
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err("export archive contains an unsafe path".to_owned());
    }
    Ok(path)
}

fn verify_tar_checksum(header: &[u8; 512]) -> Result<(), String> {
    let expected = parse_tar_octal(&header[148..156])?;
    let actual = header
        .iter()
        .enumerate()
        .map(|(index, byte)| {
            if (148..156).contains(&index) {
                u64::from(b' ')
            } else {
                u64::from(*byte)
            }
        })
        .sum::<u64>();
    if actual != expected {
        return Err("export archive header checksum is invalid".to_owned());
    }
    Ok(())
}

pub(crate) fn extract_directory_tar(
    archive: &mut (impl Read + Seek),
    destination: &Path,
    control: &ExportControl,
) -> Result<(), String> {
    control.check()?;
    let archive_bytes = archive
        .seek(SeekFrom::End(0))
        .map_err(|error| format!("inspect export archive size: {error}"))?;
    if archive_bytes > control.maximum_archive_bytes || archive_bytes % 512 != 0 {
        return Err("Export archive exceeds its size limit or is truncated".to_owned());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| "the export destination has no parent directory".to_owned())?;
    if destination.exists() {
        return Err("the selected export folder already exists".to_owned());
    }
    let name = destination
        .file_name()
        .ok_or_else(|| "the export destination has no folder name".to_owned())?;
    let staging = parent.join(format!(
        ".{}.axkdeck-extract-{}",
        name.to_string_lossy(),
        candidate_id()?
    ));
    std::fs::create_dir(&staging)
        .map_err(|error| format!("create export extraction staging folder: {error}"))?;
    let result = (|| {
        archive
            .seek(SeekFrom::Start(0))
            .map_err(|error| format!("rewind export archive: {error}"))?;
        let mut entries = 0_usize;
        let mut zero_blocks = 0_u8;
        let mut payload_bytes = 0_u64;
        let mut paths = HashSet::new();
        let mut buffer = vec![0_u8; 1024 * 1024];
        loop {
            control.check()?;
            let mut header = [0_u8; 512];
            archive
                .read_exact(&mut header)
                .map_err(|error| format!("read export archive header: {error}"))?;
            if header.iter().all(|byte| *byte == 0) {
                zero_blocks += 1;
                if zero_blocks == 2 {
                    break;
                }
                continue;
            }
            if zero_blocks != 0 {
                return Err("export archive has a malformed end marker".to_owned());
            }
            entries += 1;
            if entries > control.maximum_entries {
                return Err("export archive contains too many entries".to_owned());
            }
            verify_tar_checksum(&header)?;
            if &header[257..263] != b"ustar\0" {
                return Err("export archive is not in the supported USTAR profile".to_owned());
            }
            let relative = checked_tar_path(&header)?;
            if !paths.insert(relative.clone()) {
                return Err("Export archive contains a duplicate path".to_owned());
            }
            let output_path = staging.join(relative);
            let size = parse_tar_octal(&header[124..136])?;
            payload_bytes = payload_bytes
                .checked_add(size)
                .ok_or_else(|| "Export payload size overflow".to_owned())?;
            if payload_bytes > control.maximum_payload_bytes {
                return Err("Export payload exceeds its size limit".to_owned());
            }
            match header[156] {
                b'5' => {
                    if size != 0 {
                        return Err("export archive directory has an invalid payload".to_owned());
                    }
                    std::fs::create_dir_all(&output_path)
                        .map_err(|error| format!("create export directory: {error}"))?;
                }
                0 | b'0' => {
                    if let Some(directory) = output_path.parent() {
                        std::fs::create_dir_all(directory)
                            .map_err(|error| format!("create export directory: {error}"))?;
                    }
                    let mut output = OpenOptions::new()
                        .create_new(true)
                        .write(true)
                        .open(&output_path)
                        .map_err(|error| format!("create export file: {error}"))?;
                    let mut remaining = size;
                    while remaining != 0 {
                        control.check()?;
                        let count = usize::try_from(remaining.min(buffer.len() as u64))
                            .map_err(|_| "export archive entry size is unsupported".to_owned())?;
                        archive
                            .read_exact(&mut buffer[..count])
                            .map_err(|error| format!("read export archive payload: {error}"))?;
                        output
                            .write_all(&buffer[..count])
                            .map_err(|error| format!("write export file: {error}"))?;
                        remaining -= count as u64;
                    }
                    control.check()?;
                    output
                        .sync_all()
                        .map_err(|error| format!("flush export file: {error}"))?;
                    let padding = (512 - size % 512) % 512;
                    archive
                        .seek(SeekFrom::Current(i64::try_from(padding).map_err(|_| {
                            "export archive padding is unsupported".to_owned()
                        })?))
                        .map_err(|error| format!("skip export archive padding: {error}"))?;
                }
                _ => return Err("export archive contains an unsupported entry type".to_owned()),
            }
        }
        loop {
            control.check()?;
            let count = archive
                .read(&mut buffer)
                .map_err(|error| format!("Read export archive trailer: {error}"))?;
            if count == 0 {
                break;
            }
            if buffer[..count].iter().any(|byte| *byte != 0) {
                return Err("Export archive contains trailing entries or data".to_owned());
            }
        }
        control.check()?;
        crate::file_publication::publish_new_directory(&staging, destination)
            .map_err(|error| format!("publish export folder: {error}"))
    })();
    if result.is_err() {
        let _ = std::fs::remove_dir_all(&staging);
    }
    result
}
