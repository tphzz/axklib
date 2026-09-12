use std::ffi::OsString;
use std::path::{Path, PathBuf};
use tauri_plugin_fs::FsExt;

#[tauri::command]
pub(crate) fn native_drop_coordinates_are_logical() -> bool {
    // Wry's GTK and AppKit adapters report widget points; WebView2 reports pixels.
    cfg!(any(target_os = "linux", target_os = "macos"))
}

fn directory_names(path: &Path, limit: usize) -> Result<Vec<String>, String> {
    if limit > 10000 {
        return Err("Choose at most 10000 entries".to_owned());
    }
    let before = std::fs::symlink_metadata(path).map_err(|error| error.to_string())?;
    if !before.is_dir() || before.is_symlink() {
        return Err("Dropped path is not a regular directory".to_owned());
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if before.file_attributes() & 0x400 != 0 {
            return Err("Dropped directory must not be a reparse point".to_owned());
        }
    }
    let mut names = Vec::new();
    let mut bytes = 0usize;
    for entry in std::fs::read_dir(path).map_err(|error| error.to_string())? {
        if names.len() == limit {
            return Err("Choose at most 10000 entries".to_owned());
        }
        let name = directory_name_text(entry.map_err(|error| error.to_string())?.file_name())?;
        bytes += name.len();
        if name.encode_utf16().count() > 255 || bytes > 4194304 {
            return Err("Dropped names exceed the metadata limit".to_owned());
        }
        names.push(name);
    }
    let after = std::fs::symlink_metadata(path).map_err(|error| error.to_string())?;
    if !after.is_dir()
        || after.is_symlink()
        || before.modified().ok() != after.modified().ok()
        || before.created().ok() != after.created().ok()
    {
        return Err("Dropped directory changed while reading".to_owned());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if before.dev() != after.dev() || before.ino() != after.ino() {
            return Err("Dropped directory changed while reading".to_owned());
        }
    }
    Ok(names)
}

fn directory_name_text(name: OsString) -> Result<String, String> {
    name.into_string()
        .map_err(|_| "A dropped name cannot be represented as text".to_owned())
}

#[tauri::command]
pub(crate) async fn read_native_drop_directory(
    window: tauri::WebviewWindow,
    path: PathBuf,
    limit: usize,
) -> Result<Vec<String>, String> {
    if window.label() != "main" || !window.fs_scope().is_allowed(&path) {
        return Err("Dropped directory access is not allowed".to_owned());
    }
    let scope = window.fs_scope();
    tauri::async_runtime::spawn_blocking(move || {
        scoped_directory_names(&path, limit, || scope.is_allowed(&path))
    })
    .await
    .map_err(|error| format!("Read dropped directory worker: {error}"))?
}

fn scoped_directory_names(
    path: &Path,
    limit: usize,
    allowed: impl Fn() -> bool,
) -> Result<Vec<String>, String> {
    if !allowed() {
        return Err("Dropped directory access is not allowed".to_owned());
    }
    let names = directory_names(path, limit)?;
    if !allowed() {
        return Err("Dropped directory access changed".to_owned());
    }
    Ok(names)
}

#[cfg(test)]
mod tests {
    use super::{directory_name_text, directory_names, scoped_directory_names};

    #[test]
    fn directory_name_text_preserves_unicode() {
        let name = "Sample \u{00e9} \u{1f3b5}";
        assert_eq!(directory_name_text(name.into()).unwrap(), name);
    }

    #[cfg(any(unix, windows))]
    #[test]
    fn directory_name_text_rejects_non_unicode_without_filesystem_io() {
        #[cfg(unix)]
        let invalid = {
            use std::os::unix::ffi::OsStringExt;
            std::ffi::OsString::from_vec(vec![0xff])
        };
        #[cfg(windows)]
        let invalid = {
            use std::os::windows::ffi::OsStringExt;
            std::ffi::OsString::from_wide(&[0xd800])
        };
        assert_eq!(
            directory_name_text(invalid).unwrap_err(),
            "A dropped name cannot be represented as text"
        );
    }

    #[test]
    fn names_are_bounded_and_empty_directories_are_valid() {
        let root =
            std::env::temp_dir().join(format!("axkdeck-native-directory-{}", std::process::id()));
        std::fs::create_dir(&root).unwrap();
        assert!(
            scoped_directory_names(&root.join("DOES-NOT-EXIST"), 1, || false)
                .unwrap_err()
                .contains("not allowed")
        );
        let checks = std::cell::Cell::new(0);
        assert!(
            scoped_directory_names(&root, 1, || {
                checks.set(checks.get() + 1);
                checks.get() == 1
            })
            .unwrap_err()
            .contains("changed")
        );
        assert!(directory_names(&root, 0).unwrap().is_empty());
        std::fs::write(root.join("RAW"), []).unwrap();
        std::fs::create_dir(root.join("EMPTY")).unwrap();
        assert!(directory_names(&root, 1).is_err());
        assert!(directory_names(&root, 10001).is_err());
        let mut names = directory_names(&root, 2).unwrap();
        names.sort();
        assert_eq!(names, ["EMPTY", "RAW"]);
        assert!(directory_names(&root.join("RAW"), 2).is_err());
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(root.join("EMPTY"), root.join("LINK")).unwrap();
            assert!(directory_names(&root.join("LINK"), 2).is_err());
            std::fs::remove_file(root.join("LINK")).unwrap();
        }
        std::fs::remove_file(root.join("RAW")).unwrap();
        std::fs::remove_dir(root.join("EMPTY")).unwrap();
        std::fs::remove_dir(root).unwrap();
    }
}
