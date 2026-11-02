use super::{Filter, Fixture, LogReader, line};
use std::fs;

#[test]
fn identical_recreated_file_is_not_hidden_by_the_previous_clear_boundary() {
    let root = Fixture::new();
    let text = line("repeated");
    root.write("axkdeck.log", text.as_bytes());
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let old_id = reader.matching(&Filter::all())[0].id;
    fs::remove_file(root.0.join("axkdeck.log")).unwrap();
    root.write("axkdeck.log", text.as_bytes());
    reader.refresh().unwrap();
    let visible = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(visible.len(), 1);
    assert!(visible[0].id > old_id);
    assert_eq!(visible[0].text, text.trim_end());
}

#[cfg(windows)]
fn set_created(root: &Fixture, name: &str, ticks: u64) {
    use std::os::windows::fs::MetadataExt;
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::Storage::FileSystem::SetFileTime;

    let file = fs::OpenOptions::new()
        .write(true)
        .open(root.0.join(name))
        .unwrap();
    let old = file.metadata().unwrap().creation_time();
    let created = FILETIME {
        dwLowDateTime: ticks as u32,
        dwHighDateTime: (ticks >> 32) as u32,
    };
    // SAFETY: the writable file owns a live handle and all time pointers are valid or null.
    assert_ne!(
        unsafe {
            SetFileTime(
                file.as_raw_handle(),
                &created,
                std::ptr::null(),
                std::ptr::null(),
            )
        },
        0,
        "{}",
        std::io::Error::last_os_error()
    );
    let current = file.metadata().unwrap().creation_time();
    assert_eq!(current, ticks);
    assert_ne!(current, old);
}

#[cfg(windows)]
#[test]
fn creation_time_change_during_refresh_preserves_ids_and_clear_boundary() {
    let root = Fixture::new();
    root.write("axkdeck.log", line("old").as_bytes());
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let old_id = reader.matching(&Filter::all())[0].id;
    let mut changed = false;
    reader
        .refresh_during(|| {
            if !changed {
                changed = true;
                set_created(&root, "axkdeck.log", 133_000_000_000_000_000);
            }
        })
        .unwrap();
    assert_eq!(reader.sequence, since);
    assert_eq!(reader.history_changes, 0);
    assert_eq!(reader.matching(&Filter::all())[0].id, old_id);
    assert!(
        reader
            .matching(&Filter {
                since: Some(since),
                ..Filter::all()
            })
            .is_empty()
    );
}

#[cfg(windows)]
#[test]
fn failed_refresh_retains_proven_identity_without_publishing_records() {
    let root = Fixture::new();
    root.write("axkdeck.log", line("old").as_bytes());
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let old_id = reader.matching(&Filter::all())[0].id;
    let mut attempts = 0;
    assert!(
        reader
            .refresh_during(|| {
                attempts += 1;
                root.append("axkdeck.log", line(&format!("new {attempts}")).as_bytes());
                set_created(
                    &root,
                    "axkdeck.log",
                    133_000_000_000_000_000 + attempts * 10_000_000,
                );
            })
            .is_err()
    );
    assert_eq!(attempts, 3);
    assert_eq!(reader.sequence, since);
    assert_eq!(reader.history_changes, 0);
    assert_eq!(reader.matching(&Filter::all()).len(), 1);
    assert_eq!(reader.matching(&Filter::all())[0].id, old_id);
    assert!(
        reader
            .matching(&Filter {
                since: Some(since),
                ..Filter::all()
            })
            .is_empty()
    );
    reader.refresh().unwrap();
    assert_eq!(reader.matching(&Filter::all())[0].id, old_id);
    let visible = reader.matching(&Filter {
        since: Some(since),
        ..Filter::all()
    });
    assert_eq!(visible.len(), 3);
    assert!(visible[0].text.ends_with("new 1"));
}

#[cfg(windows)]
#[test]
fn identity_change_after_verification_uses_the_live_handles_latest_identity() {
    let root = Fixture::new();
    root.write("axkdeck.log", line("application").as_bytes());
    root.write(
        "axklib-server.log",
        b"1790575200000 [stderr] {\"level\":\"info\",\"event\":\"server\"}\n",
    );
    let mut reader = LogReader::new(root.0.clone());
    reader.refresh().unwrap();
    let since = reader.sequence;
    let old_ids: Vec<_> = reader
        .matching(&Filter::all())
        .iter()
        .map(|e| e.id)
        .collect();
    let mut changed = false;
    let mut opens = 0;
    let mut verification_first = None;
    reader
        .refresh_with_hooks(
            || {
                if !changed {
                    changed = true;
                    set_created(&root, "axkdeck.log", 133_000_000_000_000_000);
                    set_created(&root, "axklib-server.log", 133_000_000_000_000_000);
                }
            },
            |path| {
                opens += 1;
                if opens == 3 {
                    verification_first = Some(path.to_path_buf());
                } else if opens == 4 {
                    let name = verification_first
                        .as_ref()
                        .unwrap()
                        .file_name()
                        .unwrap()
                        .to_str()
                        .unwrap();
                    set_created(&root, name, 133_000_000_010_000_000);
                }
            },
        )
        .unwrap();
    assert_eq!(opens, 8);
    assert_eq!(reader.sequence, since);
    assert_eq!(reader.history_changes, 0);
    assert_eq!(
        reader
            .matching(&Filter::all())
            .iter()
            .map(|e| e.id)
            .collect::<Vec<_>>(),
        old_ids
    );
    assert!(
        reader
            .matching(&Filter {
                since: Some(since),
                ..Filter::all()
            })
            .is_empty()
    );
}
