use std::path::Path;

pub struct PublicationOutcome {
    pub warning: Option<String>,
}

#[cfg(unix)]
pub(crate) fn publish_new_directory(temporary: &Path, destination: &Path) -> std::io::Result<()> {
    use rustix::fs::{CWD, RenameFlags, renameat_with};
    renameat_with(CWD, temporary, CWD, destination, RenameFlags::NOREPLACE).map_err(Into::into)
}

#[cfg(windows)]
pub(crate) fn publish_new_directory(temporary: &Path, destination: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{MOVEFILE_WRITE_THROUGH, MoveFileExW};
    let wide = |path: &Path| {
        path.as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect::<Vec<_>>()
    };
    let temporary = wide(temporary);
    let destination = wide(destination);
    if unsafe {
        MoveFileExW(
            temporary.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn validate_destination(destination: &Path) -> Result<bool, String> {
    match destination.symlink_metadata() {
        Ok(metadata) => {
            if metadata.file_type().is_symlink() || !metadata.file_type().is_file() {
                return Err(
                    "refusing to replace a destination that is not a regular file".to_owned(),
                );
            }
            Ok(true)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(format!("inspect publication destination: {error}")),
    }
}

#[cfg(not(windows))]
pub fn publish_file(temporary: &Path, destination: &Path) -> Result<PublicationOutcome, String> {
    let _destination_exists = validate_destination(destination)?;
    std::fs::rename(temporary, destination)
        .map_err(|error| format!("publish file atomically: {error}"))?;
    let warning = destination.parent().and_then(|parent| {
        std::fs::File::open(parent)
            .and_then(|directory| directory.sync_all())
            .err()
            .map(|error| {
                format!("file was published, but its directory could not be synchronized: {error}")
            })
    });
    Ok(PublicationOutcome { warning })
}

#[cfg(windows)]
pub fn publish_file(temporary: &Path, destination: &Path) -> Result<PublicationOutcome, String> {
    use std::os::windows::ffi::OsStrExt;
    use std::ptr;

    use windows_sys::Win32::Storage::FileSystem::{
        MOVEFILE_WRITE_THROUGH, MoveFileExW, REPLACEFILE_WRITE_THROUGH, ReplaceFileW,
    };

    let destination_exists = validate_destination(destination)?;
    let wide = |path: &Path| {
        path.as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect::<Vec<_>>()
    };
    let temporary = wide(temporary);
    let destination = wide(destination);
    let published = unsafe {
        if destination_exists {
            ReplaceFileW(
                destination.as_ptr(),
                temporary.as_ptr(),
                ptr::null(),
                REPLACEFILE_WRITE_THROUGH,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        } else {
            MoveFileExW(
                temporary.as_ptr(),
                destination.as_ptr(),
                MOVEFILE_WRITE_THROUGH,
            )
        }
    };
    if published == 0 {
        return Err(format!(
            "publish file atomically: {}",
            std::io::Error::last_os_error()
        ));
    }
    Ok(PublicationOutcome { warning: None })
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::{publish_file, publish_new_directory};

    static NEXT_TEST_DIRECTORY: AtomicU64 = AtomicU64::new(0);

    fn temporary_directory() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "axkdeck-file-publication-{}-{nonce}-{}",
            std::process::id(),
            NEXT_TEST_DIRECTORY.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).expect("create test directory");
        path
    }

    #[test]
    fn directory_publication_does_not_replace_a_newly_created_empty_destination() {
        let root = temporary_directory();
        let temporary = root.join("staging");
        let destination = root.join("export");
        fs::create_dir(&temporary).unwrap();
        fs::write(temporary.join("payload"), [1, 2, 3]).unwrap();
        assert!(!destination.exists());
        fs::create_dir(&destination).unwrap();

        let result = publish_new_directory(&temporary, &destination);
        let preserved =
            temporary.join("payload").exists() && fs::read_dir(&destination).unwrap().count() == 0;
        fs::remove_dir_all(root).unwrap();
        assert!(
            result.is_err(),
            "must not replace the concurrently created directory"
        );
        assert!(preserved, "both directories must remain untouched");
    }

    #[test]
    fn directory_publication_moves_the_complete_tree_to_an_absent_destination() {
        let root = temporary_directory();
        let temporary = root.join("staging");
        let destination = root.join("export");
        fs::create_dir_all(temporary.join("empty")).unwrap();
        fs::write(temporary.join("payload"), [1, 2, 3]).unwrap();
        publish_new_directory(&temporary, &destination).unwrap();
        assert!(!temporary.exists());
        assert!(destination.join("empty").is_dir());
        assert_eq!(fs::read(destination.join("payload")).unwrap(), [1, 2, 3]);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn directory_publication_rejects_files_and_nonempty_directories() {
        let root = temporary_directory();
        let temporary = root.join("staging");
        fs::create_dir(&temporary).unwrap();
        fs::write(temporary.join("payload"), b"new").unwrap();
        let file = root.join("file");
        let directory = root.join("directory");
        fs::write(&file, b"old").unwrap();
        fs::create_dir(&directory).unwrap();
        fs::write(directory.join("old"), b"old").unwrap();
        for destination in [&file, &directory] {
            assert!(publish_new_directory(&temporary, destination).is_err());
            assert_eq!(fs::read(temporary.join("payload")).unwrap(), b"new");
        }
        assert_eq!(fs::read(file).unwrap(), b"old");
        assert_eq!(fs::read(directory.join("old")).unwrap(), b"old");
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn directory_publication_does_not_follow_or_replace_dangling_symlinks() {
        let root = temporary_directory();
        let temporary = root.join("staging");
        let destination = root.join("link");
        fs::create_dir(&temporary).unwrap();
        std::os::unix::fs::symlink("missing", &destination).unwrap();
        assert!(!destination.exists());
        assert!(publish_new_directory(&temporary, &destination).is_err());
        assert_eq!(
            fs::read_link(&destination).unwrap(),
            PathBuf::from("missing")
        );
        assert!(temporary.is_dir());
        assert!(!root.join("missing").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn atomically_replaces_a_regular_file() {
        let root = temporary_directory();
        let destination = root.join("result.axkpkg");
        let temporary = root.join(".result.tmp");
        fs::write(&destination, b"original").expect("write original");
        fs::write(&temporary, b"replacement").expect("write replacement");

        let outcome = publish_file(&temporary, &destination).expect("publish replacement");

        assert_eq!(outcome.warning, None);
        assert_eq!(
            fs::read(&destination).expect("read destination"),
            b"replacement"
        );
        assert!(!temporary.exists());
        fs::remove_dir_all(root).expect("remove test directory");
    }

    #[test]
    fn leaves_the_original_visible_when_the_candidate_is_missing() {
        let root = temporary_directory();
        let destination = root.join("result.axkpkg");
        fs::write(&destination, b"original").expect("write original");

        assert!(publish_file(&root.join("missing.tmp"), &destination).is_err());

        assert_eq!(
            fs::read(&destination).expect("read destination"),
            b"original"
        );
        fs::remove_dir_all(root).expect("remove test directory");
    }

    #[test]
    fn leaves_the_candidate_available_when_publication_is_rejected() {
        let root = temporary_directory();
        let destination = root.join("result.axkpkg");
        let temporary = root.join(".result.tmp");
        fs::create_dir(&destination).expect("create invalid destination");
        fs::write(&temporary, b"replacement").expect("write replacement");

        assert!(publish_file(&temporary, &destination).is_err());

        assert_eq!(
            fs::read(&temporary).expect("read candidate"),
            b"replacement"
        );
        fs::remove_dir_all(root).expect("remove test directory");
    }
}
