use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::SystemTime;

use super::files::{Identity, ScanError, scan};
use super::records::{Entry, Filter, Source, parse_entry, starts_record};

const MAX_FILE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_CACHE_BYTES: usize = 64 * 1024 * 1024;
const MAX_RECORD_BYTES: usize = 256 * 1024;
const MAX_ENTRIES: usize = 250_000;

pub fn log_source(name: &str) -> Option<Source> {
    if name == "axklib-server.log" || matches!(name, "axklib-server.log.1" | "axklib-server.log.2")
    {
        return Some(Source::LocalServer);
    }
    if name == "axkdeck.log" {
        return Some(Source::Application);
    }
    let suffix = name.strip_prefix("axkdeck_")?;
    let date = suffix.strip_suffix(".bak").unwrap_or(suffix);
    let date = date.strip_suffix(".log")?;
    if !date.is_empty()
        && date
            .bytes()
            .all(|c| c.is_ascii_digit() || matches!(c, b'-' | b'_' | b'T' | b'.' | b':'))
    {
        Some(Source::Application)
    } else {
        None
    }
}

#[derive(Clone)]
struct FileLog {
    identity: Identity,
    #[cfg(unix)]
    _identity_handle: Arc<File>,
    source: Source,
    offset: u64,
    modified: Option<SystemTime>,
    anchor: Vec<u8>,
    head: Vec<u8>,
    pending: Vec<u8>,
    pending_id: Option<u64>,
    entries: Vec<Arc<Entry>>,
}

impl FileLog {
    fn new(identity: Identity, source: Source, file: &File) -> Result<Self, String> {
        #[cfg(windows)]
        let _ = file;
        Ok(Self {
            identity,
            // Unix pins prevent inode reuse. Windows uses file ID + creation time:
            // a persistent handle there would delay deletion and break rotation.
            #[cfg(unix)]
            _identity_handle: Arc::new(
                file.try_clone()
                    .map_err(|e| format!("Retain log identity: {e}"))?,
            ),
            source,
            offset: 0,
            modified: None,
            anchor: Vec::new(),
            head: Vec::new(),
            pending: Vec::new(),
            pending_id: None,
            entries: Vec::new(),
        })
    }

    fn consume(&mut self, bytes: &[u8], sequence: &mut u64) -> Result<(), String> {
        for piece in bytes.split_inclusive(|value| *value == b'\n') {
            if self.pending_id.is_none() {
                *sequence += 1;
                self.pending_id = Some(*sequence);
            }
            if self.pending.len() + piece.len() > MAX_RECORD_BYTES {
                return Err("A log record exceeds the 256 KiB viewer limit".into());
            }
            self.pending.extend_from_slice(piece);
            if piece.last() != Some(&b'\n') {
                continue;
            }
            let text = String::from_utf8_lossy(&self.pending)
                .trim_end_matches(['\r', '\n'])
                .to_owned();
            let id = self.pending_id.take().unwrap();
            self.pending.clear();
            if !starts_record(self.source, &text) {
                if let Some(previous) = self.entries.last_mut() {
                    if previous.text.len() + text.len() + 1 > MAX_RECORD_BYTES {
                        return Err(
                            "A multiline log record exceeds the 256 KiB viewer limit".into()
                        );
                    }
                    let previous = Arc::make_mut(previous);
                    previous.text.push('\n');
                    previous.text.push_str(&text);
                    continue;
                }
            }
            self.entries
                .push(Arc::new(parse_entry(id, self.source, text)));
            if self.entries.len() > MAX_ENTRIES {
                return Err("The retained log exceeds the viewer entry limit".into());
            }
        }
        Ok(())
    }

    fn read(&mut self, file: &mut File, sequence: &mut u64) -> Result<bool, String> {
        let metadata = file.metadata().map_err(|e| e.to_string())?;
        let length = metadata.len();
        if length > MAX_FILE_BYTES {
            return Err("A log file exceeds the 8 MiB viewer limit".into());
        }
        let modified = metadata.modified().ok();
        if length == self.offset && modified != self.modified && self.offset != 0 {
            // A timestamp change alone must not reimport cleared records.
            let mut replacement = Self::new(self.identity, self.source, file)?;
            let mut replacement_sequence = *sequence;
            replacement.read(file, &mut replacement_sequence)?;
            let replaced = self.pending != replacement.pending
                || self.entries.len() != replacement.entries.len()
                || self
                    .entries
                    .iter()
                    .zip(&replacement.entries)
                    .any(|(old, new)| old.text != new.text);
            if replaced {
                *sequence = replacement_sequence;
            } else {
                replacement.entries = std::mem::take(&mut self.entries);
                replacement.pending_id = self.pending_id;
            }
            *self = replacement;
            return Ok(replaced);
        }
        let mut replaced = length < self.offset;
        if !replaced && self.offset > 0 {
            file.seek(SeekFrom::Start(self.offset - self.anchor.len() as u64))
                .map_err(|e| e.to_string())?;
            let mut anchor = vec![0; self.anchor.len()];
            replaced = file.read_exact(&mut anchor).is_err() || anchor != self.anchor;
            file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
            let mut head = vec![0; self.head.len()];
            replaced |= file.read_exact(&mut head).is_err() || head != self.head;
        }
        if replaced {
            *self = Self::new(self.identity, self.source, file)?;
        }
        file.seek(SeekFrom::Start(self.offset))
            .map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        file.take(length - self.offset)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        self.consume(&bytes, sequence)?;
        if self.head.len() < 64 {
            self.head.extend(bytes.iter().take(64 - self.head.len()));
        }
        self.offset += bytes.len() as u64;
        self.anchor.extend_from_slice(&bytes);
        if self.anchor.len() > 64 {
            self.anchor.drain(..self.anchor.len() - 64);
        }
        self.modified = modified;
        Ok(replaced)
    }
}

#[derive(Clone)]
pub struct LogReader {
    directory: PathBuf,
    files: Vec<FileLog>,
    pub sequence: u64,
    pub history_changes: u64,
}

impl LogReader {
    pub fn new(directory: PathBuf) -> Self {
        Self {
            directory,
            files: Vec::new(),
            sequence: 0,
            history_changes: 0,
        }
    }

    pub fn directory(&self) -> &Path {
        &self.directory
    }

    pub fn refresh(&mut self) -> Result<(), String> {
        self.refresh_during(|| {})
    }

    pub(super) fn refresh_during(&mut self, during_scan: impl FnMut()) -> Result<(), String> {
        self.refresh_with_hooks(during_scan, |_| {})
    }

    pub(super) fn refresh_with_hooks(
        &mut self,
        mut during_scan: impl FnMut(),
        mut before_open: impl FnMut(&Path),
    ) -> Result<(), String> {
        for _ in 0..3 {
            let mut opened = match scan(&self.directory, &mut before_open) {
                Ok(opened) => opened,
                Err(ScanError::Rotated) => continue,
                Err(ScanError::Failed(error)) => return Err(error),
            };
            let identities: HashMap<_, _> = opened
                .iter()
                .map(|item| (item.path.clone(), item.identity))
                .collect();
            let total_bytes = opened
                .iter()
                .try_fold(0_u64, |sum, item| {
                    item.file.metadata().map(|m| sum + m.len())
                })
                .map_err(|e| format!("Measure retained logs: {e}"))?;
            if total_bytes > MAX_CACHE_BYTES as u64 {
                return Err("Retained logs exceed the 64 MiB viewer limit".into());
            }
            let mut candidate = self.clone();
            candidate.read_files(&mut opened)?;
            during_scan();
            let current = match scan(&self.directory, &mut before_open) {
                Ok(opened) => opened,
                Err(ScanError::Rotated) => continue,
                Err(ScanError::Failed(error)) => return Err(error),
            };
            let current: HashMap<_, _> = current
                .iter()
                .map(|item| (item.path.clone(), item.identity))
                .collect();
            if identities != current {
                continue;
            }
            *self = candidate;
            return Ok(());
        }
        Err("Log files rotated repeatedly during reading. Retry to load a consistent view.".into())
    }

    fn read_files(&mut self, opened: &mut [super::files::OpenedLog]) -> Result<(), String> {
        let mut seen = HashSet::new();
        for item in opened {
            let key = item.identity;
            if !seen.insert(key) {
                continue;
            }
            let index = match self.files.iter().position(|item| item.identity == key) {
                Some(index) => index,
                None => {
                    self.files.push(FileLog::new(key, item.source, &item.file)?);
                    self.files.len() - 1
                }
            };
            let metadata = item.file.metadata().map_err(|e| e.to_string())?;
            if (metadata.len() != self.files[index].offset
                || metadata.modified().ok() != self.files[index].modified)
                && self.files[index].read(&mut item.file, &mut self.sequence)?
            {
                self.history_changes += 1;
            }
        }
        let before = self.files.len();
        self.files.retain(|item| seen.contains(&item.identity));
        if self.files.len() != before {
            self.history_changes += 1;
        }
        if self
            .files
            .iter()
            .map(|item| item.entries.len())
            .sum::<usize>()
            > MAX_ENTRIES
        {
            return Err("Retained logs exceed the 250,000 entry viewer limit".into());
        }
        let cached_bytes: usize = self
            .files
            .iter()
            .map(|file| {
                file.pending.len()
                    + file
                        .entries
                        .iter()
                        .map(|entry| entry.text.len())
                        .sum::<usize>()
            })
            .sum();
        if cached_bytes > MAX_CACHE_BYTES {
            return Err("Decoded logs exceed the 64 MiB viewer limit".into());
        }
        Ok(())
    }

    pub fn matching(&self, filter: &Filter) -> Vec<Arc<Entry>> {
        let mut entries = Vec::new();
        let mut times = [0_i64; 2];
        for file in &self.files {
            let source_index = if file.source == Source::Application {
                0
            } else {
                1
            };
            for entry in &file.entries {
                // A clock correction must not reorder records within a source.
                times[source_index] = times[source_index].max(entry.timestamp.unwrap_or(0));
                if filter.matches(entry) {
                    entries.push((times[source_index], Arc::clone(entry)));
                }
            }
        }
        entries.sort_by_key(|(time, entry)| (*time, entry.id));
        entries.into_iter().map(|(_, entry)| entry).collect()
    }
}
