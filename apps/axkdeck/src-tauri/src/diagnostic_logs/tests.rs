use super::pages::{ReadRequest, read_page};
use super::reader::{LogReader, log_source};
use super::records::Filter;
use super::records::{Level, Source, parse_entry};
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axkdeck-log-tests-{}-{}",
            std::process::id(),
            NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn write(&self, name: &str, bytes: &[u8]) {
        fs::write(self.0.join(name), bytes).unwrap();
    }
    fn append(&self, name: &str, bytes: &[u8]) {
        let mut file = fs::OpenOptions::new()
            .append(true)
            .open(self.0.join(name))
            .unwrap();
        file.write_all(bytes).unwrap();
    }
    fn set_modified(&self, name: &str, seconds: u64) {
        fs::OpenOptions::new()
            .write(true)
            .open(self.0.join(name))
            .unwrap()
            .set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(seconds))
            .unwrap();
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}

fn line(message: &str) -> String {
    format!("[2026-09-28][06:00:00][test][INFO] {message}\n")
}

#[test]
fn combines_retained_sources_and_filters_without_treating_stderr_as_errors() {
    let root = Fixture::new();
    root.write(
        "axkdeck_2026-09-27_06-00-00.log",
        line("previous run").as_bytes(),
    );
    root.write("axkdeck.log", line("new run").as_bytes());
    root.write(
        "axklib-server.log",
        b"1790575200000 [stderr] unknown event\n",
    );
    root.write("unrelated.log", b"never read\n");
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    assert_eq!(reader.matching(&Filter::all()).len(), 3);
    let filter = Filter {
        minimum_level: Some(Level::Warning),
        ..Filter::all()
    };
    assert_eq!(reader.matching(&filter).len(), 1);
    assert_eq!(
        reader
            .matching(&Filter {
                include_unclassified: false,
                ..filter
            })
            .len(),
        0
    );
    assert_eq!(
        reader
            .matching(&Filter {
                search: "PREVIOUS".into(),
                source: Some(Source::Application),
                ..Filter::all()
            })
            .len(),
        1
    );
}

#[test]
fn clear_boundary_survives_identical_timestamps_partial_writes_and_rotation() {
    let root = Fixture::new();
    root.write(
        "axkdeck.log",
        format!("{}[2026-09-28][06:00:00][test][INFO] part", line("old")).as_bytes(),
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let old_id = reader.matching(&Filter::all())[0].id;
    root.append("axkdeck.log", b"ial\n");
    fs::rename(
        root.0.join("axkdeck.log"),
        root.0.join("axkdeck_2026-09-28_06-00-00.log"),
    )
    .unwrap();
    root.write("axkdeck.log", line("new").as_bytes());
    reader.refresh().unwrap();
    reader.refresh().unwrap();
    assert_eq!(reader.matching(&Filter::all()).len(), 3);
    assert_eq!(reader.matching(&Filter::all())[0].id, old_id);
    let cleared = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(cleared.len(), 1);
    assert!(cleared[0].text.ends_with("new"));
}

#[test]
fn continuation_lines_keep_parent_severity_and_invalid_utf8_remains_visible() {
    let root = Fixture::new();
    root.write(
        "axkdeck.log",
        b"[2026-09-28][06:00:00][test][ERROR] failed\n  stack\xff\n",
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let records = reader.matching(&Filter::all());
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].level, Some(Level::Error));
    assert!(records[0].text.contains("\n  stack\u{fffd}"));
}

#[test]
fn pagination_covers_all_entries_and_reports_expired_history() {
    let root = Fixture::new();
    root.write(
        "axkdeck.log",
        (0..850)
            .map(|i| line(&format!("record {i}")))
            .collect::<String>()
            .as_bytes(),
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let request = ReadRequest {
        filter: Filter::all(),
        ..ReadRequest::default()
    };
    let last = read_page(&reader, &request).unwrap();
    assert_eq!(last.total, 850);
    assert_eq!(last.entries.len(), 400);
    let middle = read_page(
        &reader,
        &ReadRequest {
            before: last.older_cursor,
            ..request.clone()
        },
    )
    .unwrap();
    assert_eq!(middle.entries.len(), 400);
    let first = read_page(
        &reader,
        &ReadRequest {
            before: middle.older_cursor,
            ..request.clone()
        },
    )
    .unwrap();
    assert_eq!(first.entries.len(), 50);
    let forward = read_page(
        &reader,
        &ReadRequest {
            after: first.newer_cursor,
            ..request.clone()
        },
    )
    .unwrap();
    assert_eq!(forward.entries[0].id, middle.entries[0].id);
    root.write("axkdeck.log", line("replacement").as_bytes());
    reader.refresh().unwrap();
    assert!(reader.history_changes > 0);
    assert!(
        read_page(
            &reader,
            &ReadRequest {
                before: last.older_cursor,
                ..request
            }
        )
        .is_err()
    );
}

#[test]
fn accepts_only_owned_log_names_and_rejects_symlinks() {
    assert_eq!(log_source("axkdeck.log"), Some(Source::Application));
    assert_eq!(
        log_source("axkdeck_2026-09-28_06-00-00.log.bak"),
        Some(Source::Application)
    );
    for name in [
        "../axkdeck.log",
        "axkdeck_credentials.log",
        "axklib-server.log.20",
        "axkdeck.log.bak",
    ] {
        assert_eq!(log_source(name), None);
    }
    #[cfg(unix)]
    {
        let root = Fixture::new();
        root.write("secret", b"secret\n");
        std::os::unix::fs::symlink(root.0.join("secret"), root.0.join("axkdeck.log")).unwrap();
        let mut reader = LogReader::new(root.0.clone());
        reader.refresh().unwrap();
        assert!(reader.matching(&Filter::all()).is_empty());
    }
}

#[test]
fn rotation_during_refresh_never_reimports_cleared_entries_with_new_ids() {
    for timestamp_changes in [false, true] {
        rotation_during_refresh(timestamp_changes);
    }
}

fn rotation_during_refresh(timestamp_changes: bool) {
    let root = Fixture::new();
    root.write("axklib-server.log.1", b"1790575199000 [stderr] archive\n");
    root.write("axklib-server.log", b"1790575200000 [stderr] old active\n");
    root.set_modified("axklib-server.log.1", 1790575200);
    root.set_modified("axklib-server.log", 1790575200);
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let ids: Vec<_> = reader
        .matching(&Filter::all())
        .iter()
        .map(|e| e.id)
        .collect();
    let mut rotated = false;
    reader
        .refresh_during(|| {
            if rotated {
                return;
            }
            rotated = true;
            fs::rename(
                root.0.join("axklib-server.log.1"),
                root.0.join("axklib-server.log.2"),
            )
            .unwrap();
            fs::rename(
                root.0.join("axklib-server.log"),
                root.0.join("axklib-server.log.1"),
            )
            .unwrap();
            if timestamp_changes {
                root.set_modified("axklib-server.log.1", 1790575201);
            }
            root.write("axklib-server.log", b"1790575200000 [stderr] new active\n");
        })
        .unwrap();
    assert_eq!(
        reader
            .matching(&Filter::all())
            .iter()
            .take(2)
            .map(|e| e.id)
            .collect::<Vec<_>>(),
        ids
    );
    assert_eq!(
        reader
            .matching(&Filter {
                since: Some(since),
                ..Filter::all()
            })
            .len(),
        1
    );
}

#[test]
fn modification_time_changes_preserve_cleared_entries_and_pending_record_ids() {
    let root = Fixture::new();
    root.write("axkdeck.log", format!("{}partial", line("old")).as_bytes());
    root.set_modified("axkdeck.log", 1790575200);
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let id = reader.matching(&Filter::all())[0].id;
    root.set_modified("axkdeck.log", 1790575201);
    reader.refresh().unwrap();
    assert_eq!(reader.sequence, since);
    assert_eq!(reader.history_changes, 0);
    assert_eq!(reader.matching(&Filter::all())[0].id, id);
    assert!(
        reader
            .matching(&Filter {
                since: Some(since),
                ..Filter::all()
            })
            .is_empty()
    );
    root.append("axkdeck.log", b" continuation\n");
    reader.refresh().unwrap();
    assert_eq!(reader.sequence, since);
    assert_eq!(reader.matching(&Filter::all())[0].id, id);
    assert!(
        reader.matching(&Filter::all())[0]
            .text
            .ends_with("partial continuation")
    );
}

#[test]
fn same_length_rewrite_with_unchanged_head_and_tail_replaces_cached_records() {
    let root = Fixture::new();
    let old = format!(
        "{}{}{}",
        line(&"a".repeat(100)),
        line("old"),
        line(&"z".repeat(100))
    );
    let new = old.replace("old", "new");
    assert_eq!(old.len(), new.len());
    assert_eq!(&old.as_bytes()[..64], &new.as_bytes()[..64]);
    assert_eq!(
        &old.as_bytes()[old.len() - 64..],
        &new.as_bytes()[new.len() - 64..]
    );
    root.write("axkdeck.log", old.as_bytes());
    root.set_modified("axkdeck.log", 1790575200);
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    root.write("axkdeck.log", new.as_bytes());
    root.set_modified("axkdeck.log", 1790575201);
    reader.refresh().unwrap();
    assert_eq!(reader.history_changes, 1);
    let visible = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(visible.len(), 3);
    assert!(visible[1].text.ends_with("new"));
}

#[test]
fn failed_refreshes_do_not_publish_partial_cache_or_advance_clear_boundary() {
    let root = Fixture::new();
    root.write("axkdeck.log", line("initial").as_bytes());
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    root.write("axklib-server.log", &vec![b'x'; 256 * 1024 + 1]);
    for i in 0..20 {
        fs::rename(
            root.0.join("axkdeck.log"),
            root.0.join("axkdeck_2026-09-28_00-00-00.log"),
        )
        .unwrap();
        root.write("axkdeck.log", line(&format!("rotated {i}")).as_bytes());
        assert!(reader.refresh().is_err());
        assert_eq!(reader.sequence, since);
        assert_eq!(reader.matching(&Filter::all()).len(), 1);
    }
}

#[test]
fn retries_when_rotation_removes_an_enumerated_file_before_open() {
    for rotation_scan in [1, 2] {
        let root = Fixture::new();
        root.write("axkdeck.log", line("old").as_bytes());
        let mut reader = LogReader::new(root.0.clone());
        reader.refresh().unwrap();
        let since = reader.sequence;
        let mut opens = 0;
        reader
            .refresh_with_hooks(
                || {},
                |path| {
                    opens += 1;
                    if opens == rotation_scan {
                        fs::rename(path, root.0.join("axkdeck_2026-09-28_06-00-00.log")).unwrap();
                    }
                },
            )
            .unwrap();
        assert_eq!(reader.sequence, since);
        assert_eq!(reader.matching(&Filter::all()).len(), 1);
        assert!(
            reader
                .matching(&Filter {
                    since: Some(since),
                    ..Filter::all()
                })
                .is_empty()
        );
    }
}

#[test]
fn malformed_large_timestamp_does_not_drop_original_record() {
    let text = "9223372036854775807 [stderr] bad timestamp";
    let entry = parse_entry(1, Source::LocalServer, text.into());
    assert_eq!(entry.text, text);
    assert_eq!(entry.timestamp, Some(i64::MAX));
}

#[test]
fn longer_rewrite_with_shared_suffix_is_not_mistaken_for_append() {
    let root = Fixture::new();
    let suffix = "repeated message ".repeat(20);
    root.write(
        "axklib-server.log",
        format!("1790575200000 [stderr] {suffix}\n").as_bytes(),
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    root.write(
        "axklib-server.log",
        format!("1790575200001 [stderr] {suffix}\n1790575200002 [stderr] {suffix}\n").as_bytes(),
    );
    reader.refresh().unwrap();
    let visible = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(visible.len(), 2);
    assert_eq!(visible[0].timestamp, Some(1790575200001));
}

#[test]
fn deleting_cached_files_then_recreating_keeps_all_new_records_visible() {
    let root = Fixture::new();
    root.write("axkdeck.log", line("old").as_bytes());
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    fs::remove_file(root.0.join("axkdeck.log")).unwrap();
    root.write(
        "axkdeck.log",
        format!("{}{}", line("new one"), line("new two")).as_bytes(),
    );
    reader.refresh().unwrap();
    let visible = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(visible.len(), 2);
    assert!(visible[0].text.ends_with("new one"));
}

#[test]
fn frozen_export_covers_more_than_a_page_and_ignores_later_continuations() {
    let root = Fixture::new();
    root.write(
        "axkdeck.log",
        (0..850)
            .map(|i| line(&format!("record {i}")))
            .collect::<String>()
            .as_bytes(),
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let snapshot = reader.matching(&Filter::all());
    root.append("axkdeck.log", b"later continuation\n");
    reader.refresh().unwrap();
    let destination = root.0.join("export.log");
    assert_eq!(
        super::export::write_snapshot(&destination, &snapshot).unwrap(),
        None
    );
    let text = fs::read_to_string(destination).unwrap();
    assert_eq!(text.lines().count(), 850);
    assert!(text.contains("[Application] [2026-09-28]"));
    assert!(!text.contains("later continuation"));
}

#[test]
fn export_failure_removes_staging_files_and_preserves_destination() {
    let root = Fixture::new();
    let destination = root.0.join("export.log");
    fs::create_dir(&destination).unwrap();
    fs::write(destination.join("preserved"), b"unchanged").unwrap();
    assert!(super::export::write_snapshot(&destination, &[]).is_err());
    assert_eq!(
        fs::read(destination.join("preserved")).unwrap(),
        b"unchanged"
    );
    assert_eq!(fs::read_dir(&root.0).unwrap().count(), 1);
}

#[cfg(unix)]
#[test]
fn rejects_log_symlink_substitution_and_binds_export_to_resolved_parent() {
    let root = Fixture::new();
    let logs = root.0.join("logs");
    let exports = root.0.join("exports");
    fs::create_dir(&logs).unwrap();
    fs::create_dir(&exports).unwrap();
    let alias = root.0.join("alias");
    std::os::unix::fs::symlink(&logs, &alias).unwrap();
    assert!(super::export::normalize_destination(alias.join("axkdeck.log"), &logs).is_err());
    fs::remove_file(&alias).unwrap();
    std::os::unix::fs::symlink(&exports, &alias).unwrap();
    let destination = super::export::normalize_destination(alias.join("export"), &logs).unwrap();
    fs::remove_file(&alias).unwrap();
    std::os::unix::fs::symlink(&logs, &alias).unwrap();
    super::export::write_snapshot(&destination, &[]).unwrap();
    assert!(exports.join("export.log").is_file());
    assert!(!logs.join("export.log").exists());
    root.write("secret", b"secret");
    std::os::unix::fs::symlink(root.0.join("secret"), logs.join("axkdeck.log")).unwrap();
    assert!(super::files::open_log(&logs.join("axkdeck.log")).is_err());
}

#[test]
fn application_records_preserve_text_and_decode_explicit_level_and_utc_time() {
    let text = "[2026-09-28][06:00:00][webview:writeLog][WARN] failure\n  at handler";
    let record = parse_entry(3, Source::Application, text.to_owned());
    assert_eq!(record.id, 3);
    assert_eq!(record.source, Source::Application);
    assert_eq!(record.level, Some(Level::Warning));
    assert_eq!(record.timestamp, Some(1_790_575_200_000));
    assert_eq!(record.text, text);
}

#[test]
fn stderr_is_not_error_severity() {
    let info = parse_entry(
        1,
        Source::LocalServer,
        "1790575200000 [stderr] (2026-09-28 06:00:00) [INFO    ] Request: /api/v1/jobs".into(),
    );
    assert_eq!(info.level, Some(Level::Info));
    assert_eq!(info.timestamp, Some(1_790_575_200_000));
    let unknown = parse_entry(
        2,
        Source::LocalServer,
        "1790575200001 [stderr] {\"event\":\"http_request\",\"status\":500}".into(),
    );
    assert_eq!(unknown.level, None);
}

#[test]
fn structured_severity_is_recognized_without_scanning_message_text() {
    let error = parse_entry(
        1,
        Source::LocalServer,
        "1790575200000 [stdout] {\"level\":\"error\",\"message\":\"failed\"}".into(),
    );
    assert_eq!(error.level, Some(Level::Error));
    let unknown = parse_entry(
        2,
        Source::LocalServer,
        "1790575200000 [stderr] user filename [ERROR]".into(),
    );
    assert_eq!(unknown.level, None);
}
