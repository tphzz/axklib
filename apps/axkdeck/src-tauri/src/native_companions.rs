use serde::Serialize;
use std::collections::HashSet;
#[cfg(unix)]
use std::fs::File;
use std::fs::Metadata;
use std::path::{Component, Path, PathBuf};
use tauri::{Manager, WebviewWindow};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_fs::FsExt;

const MAXIMUM_SOURCE_BYTES: u64 = 4 * 1024 * 1024;
const MAXIMUM_CANDIDATE_BYTES: u64 = 64 * 1024 * 1024;
const MAXIMUM_DIRECTORY_ENTRIES: usize = 1024;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct NativeDiskSource {
    path: String,
    identity: String,
    size: u64,
}

fn main_window(label: &str) -> Result<(), String> {
    if label == "main" {
        Ok(())
    } else {
        Err("Native disk sources are restricted to the main window".to_owned())
    }
}

fn linked(metadata: &Metadata) -> bool {
    if metadata.is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 {
            return true;
        }
    }
    false
}

fn safe_path(path: &Path) -> Result<PathBuf, String> {
    if !path.is_absolute() {
        return Err("Native disk source must be an absolute authorized path".to_owned());
    }
    let mut checked = PathBuf::new();
    for part in path.components() {
        if matches!(part, Component::ParentDir | Component::CurDir) {
            return Err("Native disk source contains an invalid path component".to_owned());
        }
        checked.push(part.as_os_str());
        if matches!(part, Component::Prefix(_)) {
            continue;
        }
        let metadata = std::fs::symlink_metadata(&checked).map_err(|error| error.to_string())?;
        if linked(&metadata) {
            return Err("Native disk sources must not traverse links or reparse points".to_owned());
        }
    }
    path.canonicalize().map_err(|error| error.to_string())
}

#[cfg(unix)]
fn identity(path: &Path) -> Result<String, String> {
    use std::os::unix::fs::MetadataExt;
    let file: File = rustix::fs::open(
        path,
        rustix::fs::OFlags::RDONLY | rustix::fs::OFlags::NOFOLLOW | rustix::fs::OFlags::CLOEXEC,
        rustix::fs::Mode::empty(),
    )
    .map_err(|error| error.to_string())?
    .into();
    let metadata = file.metadata().map_err(|error| error.to_string())?;
    Ok(format!("native:{}:{}", metadata.dev(), metadata.ino()))
}

#[cfg(windows)]
fn identity(path: &Path) -> Result<String, String> {
    use std::os::windows::fs::OpenOptionsExt;
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT,
        FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE,
        GetFileInformationByHandle,
    };
    let file = std::fs::OpenOptions::new()
        .access_mode(FILE_READ_ATTRIBUTES)
        .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)
        .map_err(|error| error.to_string())?;
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) } == 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(format!(
        "native:{}:{}:{}",
        info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow
    ))
}

fn disk_source(path: &Path) -> Result<NativeDiskSource, String> {
    let path = safe_path(path)?;
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("");
    if !matches!(extension.to_ascii_lowercase().as_str(), "img" | "ima") {
        return Err("Choose a floppy .img or .ima file".to_owned());
    }
    let before = std::fs::symlink_metadata(&path).map_err(|error| error.to_string())?;
    if !before.is_file() || linked(&before) || before.len() > MAXIMUM_SOURCE_BYTES {
        return Err("Native companion source must be a regular file of at most 4 MiB".to_owned());
    }
    let key = identity(&path)?;
    let after = std::fs::symlink_metadata(&path).map_err(|error| error.to_string())?;
    if linked(&after)
        || !after.is_file()
        || !same_metadata(&before, &after)
        || key != identity(&path)?
    {
        return Err("Native disk source changed during admission".to_owned());
    }
    Ok(NativeDiskSource {
        path: path
            .into_os_string()
            .into_string()
            .map_err(|_| "A disk path cannot be represented as text".to_owned())?,
        identity: key,
        size: after.len(),
    })
}

fn same_metadata(before: &Metadata, after: &Metadata) -> bool {
    before.len() == after.len()
        && before.modified().ok() == after.modified().ok()
        && before.created().ok() == after.created().ok()
}

fn scoped_source(path: &Path, allowed: impl Fn() -> bool) -> Result<NativeDiskSource, String> {
    if !allowed() {
        return Err("Native disk source access is not allowed".to_owned());
    }
    let source = disk_source(path)?;
    if !allowed() {
        return Err("Native disk source access changed".to_owned());
    }
    Ok(source)
}

fn siblings(
    path: &Path,
    limit: usize,
    allowed: impl Fn() -> bool,
) -> Result<Vec<NativeDiskSource>, String> {
    let seed = scoped_source(path, &allowed)?;
    let parent = Path::new(&seed.path)
        .parent()
        .ok_or("Native disk source has no parent")?;
    let before = std::fs::symlink_metadata(parent).map_err(|error| error.to_string())?;
    let parent_id = identity(parent)?;
    let mut seen = HashSet::from([seed.identity.clone()]);
    let mut result = Vec::new();
    let mut bytes = 0;
    for (index, entry) in std::fs::read_dir(parent)
        .map_err(|error| error.to_string())?
        .enumerate()
    {
        if index >= limit {
            return Err("Companion directory scan exceeds 1024 entries".to_owned());
        }
        if !allowed() {
            return Err("Native disk source access changed".to_owned());
        }
        let entry = entry.map_err(|error| error.to_string())?;
        let metadata =
            std::fs::symlink_metadata(entry.path()).map_err(|error| error.to_string())?;
        if linked(&metadata) || !metadata.is_file() || metadata.len() > MAXIMUM_SOURCE_BYTES {
            continue;
        }
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| "A disk filename cannot be represented as text".to_owned())?;
        if !name.rsplit_once('.').is_some_and(|(_, extension)| {
            matches!(extension.to_ascii_lowercase().as_str(), "img" | "ima")
        }) {
            continue;
        }
        if name.encode_utf16().count() > 255 {
            return Err("Companion filename exceeds the metadata limit".to_owned());
        }
        let candidate = disk_source(&entry.path())?;
        if !seen.insert(candidate.identity.clone()) {
            continue;
        }
        bytes += candidate.size;
        if bytes > MAXIMUM_CANDIDATE_BYTES {
            return Err("Companion discovery exceeds 64 MiB".to_owned());
        }
        result.push(candidate);
    }
    let after = std::fs::symlink_metadata(parent).map_err(|error| error.to_string())?;
    if linked(&after)
        || !same_metadata(&before, &after)
        || parent_id != identity(parent)?
        || seed.identity != scoped_source(path, allowed)?.identity
    {
        return Err("Companion directory changed during discovery".to_owned());
    }
    Ok(result)
}

fn allow_file(window: &WebviewWindow, source: &NativeDiskSource) -> Result<(), String> {
    window
        .fs_scope()
        .allow_file(&source.path)
        .map_err(|error| error.to_string())?;
    window
        .state::<tauri::scope::Scopes>()
        .allow_file(&source.path)
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub(crate) async fn inspect_native_disk_source(
    window: WebviewWindow,
    path: PathBuf,
) -> Result<NativeDiskSource, String> {
    main_window(window.label())?;
    let scope = window.fs_scope();
    let source = tauri::async_runtime::spawn_blocking(move || {
        scoped_source(&path, || scope.is_allowed(&path))
    })
    .await
    .map_err(|error| error.to_string())??;
    allow_file(&window, &source)?;
    Ok(source)
}

#[tauri::command]
pub(crate) async fn nearby_native_disk_sources(
    window: WebviewWindow,
    path: PathBuf,
) -> Result<Vec<NativeDiskSource>, String> {
    main_window(window.label())?;
    let scope = window.fs_scope();
    let sources = tauri::async_runtime::spawn_blocking(move || {
        siblings(&path, MAXIMUM_DIRECTORY_ENTRIES, || scope.is_allowed(&path))
    })
    .await
    .map_err(|error| error.to_string())??;
    for source in &sources {
        allow_file(&window, source)?;
    }
    Ok(sources)
}

#[tauri::command]
pub(crate) async fn select_native_disk_sources(
    app: tauri::AppHandle,
    window: WebviewWindow,
) -> Result<Vec<NativeDiskSource>, String> {
    main_window(window.label())?;
    let picker_window = window.clone();
    let selected = tauri::async_runtime::spawn_blocking(move || {
        app.dialog()
            .file()
            .set_title("Add floppy disks")
            .add_filter("Floppy images", &["img", "ima"])
            .set_parent(&picker_window)
            .blocking_pick_files()
    })
    .await
    .map_err(|error| error.to_string())?
    .unwrap_or_default();
    if selected.len() > 32 {
        return Err("Choose at most 32 floppy images".to_owned());
    }
    let mut sources = Vec::new();
    let mut seen = HashSet::new();
    for selection in selected {
        let path = selection
            .into_path()
            .map_err(|_| "Choose a local floppy image".to_owned())?;
        let source = disk_source(&path)?;
        if seen.insert(source.identity.clone()) {
            sources.push(source);
        }
    }
    for source in &sources {
        allow_file(&window, source)?;
    }
    Ok(sources)
}

#[cfg(test)]
#[path = "native_companions_tests.rs"]
mod tests;
