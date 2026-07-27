use std::path::Path;

pub struct PublicationOutcome {
    pub warning: Option<String>,
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
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::publish_file;

    fn temporary_directory() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "axkdeck-file-publication-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir_all(&path).expect("create test directory");
        path
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
