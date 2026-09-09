use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;

use fs2::FileExt;
use serde::{Deserialize, Serialize};

use crate::directory_tar::ExportControl;
use crate::private_directory::{create_private_directory, validate_private_directory};

#[derive(Clone, Copy)]
pub(crate) struct Limits {
    pub bytes: u64,
    pub entries: usize,
    pub ready_seconds: u64,
    pub retained_seconds: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            bytes: 8 * 1024 * 1024 * 1024,
            entries: 16,
            ready_seconds: 600,
            retained_seconds: 24 * 60 * 60,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Serialize, Deserialize)]
enum Phase {
    Reserved,
    Preparing,
    Ready,
    HandedOff,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
    reserved_bytes: u64,
    expires: u64,
    phase: Phase,
}

struct Live {
    _lease: File,
    record: Record,
    control: ExportControl,
}

pub(crate) struct Preparation {
    pub id: String,
    pub destination: PathBuf,
    pub control: ExportControl,
}

pub(crate) struct DragCache {
    root: PathBuf,
    limits: Limits,
    live: HashMap<String, Live>,
}

fn failure(error: impl std::fmt::Display) -> String {
    format!("Native drag export cache: {error}")
}

fn lock_file(path: &Path) -> Result<File, String> {
    if let Ok(metadata) = std::fs::symlink_metadata(path) {
        if !metadata.is_file() || metadata.file_type().is_symlink() {
            return Err(failure("invalid lock file"));
        }
    }
    OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(path)
        .map_err(failure)
}

fn write_record(path: &Path, record: &Record) -> Result<(), String> {
    let bytes = serde_json::to_vec(record).map_err(failure)?;
    let temporary = path.join("record.tmp");
    let mut file = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(&temporary)
        .map_err(failure)?;
    file.write_all(&bytes).map_err(failure)?;
    file.sync_all().map_err(failure)?;
    drop(file);
    let outcome = crate::file_publication::publish_file(&temporary, &path.join("record.json"))?;
    if let Some(warning) = outcome.warning {
        log::warn!("{warning}");
    }
    Ok(())
}

fn read_record(path: &Path) -> Result<Record, String> {
    let mut bytes = Vec::new();
    File::open(path.join("record.json"))
        .map_err(failure)?
        .take(1025)
        .read_to_end(&mut bytes)
        .map_err(failure)?;
    if bytes.len() > 1024 {
        return Err(failure("oversized cache record"));
    }
    serde_json::from_slice(&bytes).map_err(failure)
}

impl DragCache {
    pub fn open(root: &Path, limits: Limits) -> Result<Self, String> {
        match create_private_directory(root) {
            Ok(()) => (),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => (),
            Err(error) => return Err(failure(error)),
        }
        validate_private_directory(root)?;
        Ok(Self {
            root: root.canonicalize().map_err(failure)?,
            limits,
            live: HashMap::new(),
        })
    }

    fn guard(&self) -> Result<File, String> {
        validate_private_directory(&self.root)?;
        let file = lock_file(&self.root.join("cache.lock"))?;
        FileExt::lock_exclusive(&file).map_err(failure)?;
        Ok(file)
    }

    // The global lock serializes accounting. Each live worker/OS drag holds a
    // separate lease, so another process cannot expire files still being used.
    fn sweep(&mut self, now: u64) -> Result<(u64, usize), String> {
        let expired: Vec<_> = self
            .live
            .iter()
            .filter(|(_, live)| {
                matches!(live.record.phase, Phase::Reserved | Phase::Ready)
                    && live.record.expires <= now
            })
            .map(|(id, _)| id.clone())
            .collect();
        for id in expired {
            self.live.remove(&id);
        }
        let mut bytes = 0_u64;
        let mut count = 0_usize;
        for entry in std::fs::read_dir(&self.root).map_err(failure)? {
            let entry = entry.map_err(failure)?;
            let name = entry.file_name();
            if name == "cache.lock" {
                continue;
            }
            let name = name
                .to_str()
                .ok_or_else(|| failure("invalid cache entry"))?;
            if name.len() != 32 || !name.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                return Err(failure("unexpected cache entry"));
            }
            count += 1;
            if count > 256 {
                return Err(failure("too many cache records"));
            }
            let path = entry.path();
            validate_private_directory(&path)?;
            let lease = lock_file(&path.join("lease"))?;
            let unlocked = match FileExt::try_lock_exclusive(&lease) {
                Ok(()) => true,
                Err(error)
                    if error.raw_os_error() == fs2::lock_contended_error().raw_os_error() =>
                {
                    false
                }
                Err(error) => return Err(failure(error)),
            };
            let record = read_record(&path);
            if unlocked
                && (!path.join("record.json").exists()
                    || record.as_ref().is_ok_and(|record| record.expires <= now))
            {
                drop(lease);
                std::fs::remove_dir_all(path).map_err(failure)?;
                count -= 1;
                continue;
            }
            let record = record?;
            bytes = bytes
                .checked_add(record.reserved_bytes)
                .ok_or_else(|| failure("cache size overflow"))?;
        }
        Ok((bytes, count))
    }

    pub fn prune(&mut self, now: u64) -> Result<(), String> {
        let _guard = self.guard()?;
        self.sweep(now).map(|_| ())
    }

    pub fn reserved(&self, id: &str) -> bool {
        self.live
            .get(id)
            .is_some_and(|live| live.record.phase == Phase::Reserved)
    }

    pub fn reserve(&mut self, archive_bytes: u64, now: u64) -> Result<Preparation, String> {
        let mut control = ExportControl::default();
        if archive_bytes < 1024
            || archive_bytes % 512 != 0
            || archive_bytes > control.maximum_archive_bytes
        {
            return Err(failure("unsupported archive size"));
        }
        let reserved_bytes = archive_bytes
            .checked_mul(2)
            .ok_or_else(|| failure("cache size overflow"))?;
        control.maximum_archive_bytes = archive_bytes;
        control.maximum_payload_bytes = archive_bytes;
        let _guard = self.guard()?;
        let (bytes, count) = self.sweep(now)?;
        if count >= self.limits.entries
            || bytes
                .checked_add(reserved_bytes)
                .is_none_or(|sum| sum > self.limits.bytes)
        {
            return Err(failure(
                "temporary export limit reached; use Export to disk instead",
            ));
        }
        let mut random = [0_u8; 16];
        getrandom::fill(&mut random).map_err(failure)?;
        let id: String = random.iter().map(|byte| format!("{byte:02x}")).collect();
        let path = self.root.join(&id);
        create_private_directory(&path).map_err(failure)?;
        let result = (|| {
            let lease = lock_file(&path.join("lease"))?;
            FileExt::lock_exclusive(&lease).map_err(failure)?;
            let record = Record {
                reserved_bytes,
                expires: now.saturating_add(self.limits.ready_seconds),
                phase: Phase::Reserved,
            };
            write_record(&path, &record)?;
            self.live.insert(
                id.clone(),
                Live {
                    _lease: lease,
                    record,
                    control: control.clone(),
                },
            );
            Ok(Preparation {
                id,
                destination: path.join("files"),
                control,
            })
        })();
        if result.is_err() {
            let _ = std::fs::remove_dir_all(path);
        }
        result
    }

    pub fn cancel(&mut self, id: &str) -> Result<(), String> {
        let _guard = self.guard()?;
        let Some(live) = self.live.get(id) else {
            return Ok(());
        };
        if live.record.phase == Phase::HandedOff {
            return Ok(());
        }
        live.control.cancelled.store(true, Ordering::Relaxed);
        if matches!(live.record.phase, Phase::Ready | Phase::Reserved) {
            self.live.remove(id);
            std::fs::remove_dir_all(self.root.join(id)).map_err(failure)?;
        }
        Ok(())
    }

    pub fn begin(&mut self, id: &str) -> Result<Preparation, String> {
        let _guard = self.guard()?;
        let live = self
            .live
            .get_mut(id)
            .ok_or_else(|| failure("preparation expired"))?;
        if live.record.phase != Phase::Reserved {
            return Err(failure("preparation already started"));
        }
        live.control.check()?;
        live.record.phase = Phase::Preparing;
        write_record(&self.root.join(id), &live.record)?;
        Ok(Preparation {
            id: id.to_owned(),
            destination: self.root.join(id).join("files"),
            control: live.control.clone(),
        })
    }

    pub fn finish(&mut self, id: &str, success: bool, now: u64) -> Result<(), String> {
        let _guard = self.guard()?;
        let live = self
            .live
            .get_mut(id)
            .ok_or_else(|| failure("preparation expired"))?;
        if live.record.phase != Phase::Preparing {
            return Err(failure("preparation already finished"));
        }
        if !success || live.control.check().is_err() {
            self.live.remove(id);
            return std::fs::remove_dir_all(self.root.join(id)).map_err(failure);
        }
        live.record.phase = Phase::Ready;
        live.record.expires = now.saturating_add(self.limits.ready_seconds);
        write_record(&self.root.join(id), &live.record)
    }

    pub fn claim(&mut self, id: &str, now: u64) -> Result<Vec<PathBuf>, String> {
        let _guard = self.guard()?;
        let live = self
            .live
            .get_mut(id)
            .ok_or_else(|| failure("preparation expired"))?;
        if live.record.phase != Phase::Ready || live.record.expires <= now {
            return Err(failure("export is not ready"));
        }
        live.control.check()?;
        let mut paths = Vec::new();
        for entry in std::fs::read_dir(self.root.join(id).join("files")).map_err(failure)? {
            let entry = entry.map_err(failure)?;
            let kind = entry.file_type().map_err(failure)?;
            if !(kind.is_file() || kind.is_dir()) || paths.len() >= 100_000 {
                return Err(failure("invalid prepared files"));
            }
            paths.push(entry.path());
        }
        if paths.is_empty() {
            return Err(failure("no files to drag"));
        }
        paths.sort();
        live.record.phase = Phase::HandedOff;
        live.record.expires = now.saturating_add(self.limits.retained_seconds);
        write_record(&self.root.join(id), &live.record)?;
        Ok(paths)
    }

    pub fn end(&mut self, id: &str, dropped: bool) -> Result<(), String> {
        let _guard = self.guard()?;
        if self
            .live
            .get(id)
            .is_none_or(|live| live.record.phase != Phase::HandedOff)
        {
            return Err(failure("drag is not active"));
        }
        self.live.remove(id);
        if !dropped {
            std::fs::remove_dir_all(self.root.join(id)).map_err(failure)?;
        }
        Ok(())
    }
}
