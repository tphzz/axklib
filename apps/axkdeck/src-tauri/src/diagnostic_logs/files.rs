use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use super::reader::log_source;
use super::records::Source;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Identity(u64, u128, u64);

#[cfg(unix)]
fn identity(file: &File) -> std::io::Result<Identity> {
    use std::os::unix::fs::MetadataExt;
    let metadata = file.metadata()?;
    Ok(Identity(metadata.dev(), u128::from(metadata.ino()), 0))
}

#[cfg(windows)]
fn identity(file: &File) -> std::io::Result<Identity> {
    use std::os::windows::fs::MetadataExt;
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        FILE_ID_INFO, FileIdInfo, GetFileInformationByHandleEx,
    };
    let mut info = std::mem::MaybeUninit::<FILE_ID_INFO>::uninit();
    // SAFETY: the file owns a live handle and the API initializes the output on success.
    if unsafe {
        GetFileInformationByHandleEx(
            file.as_raw_handle(),
            FileIdInfo,
            info.as_mut_ptr().cast(),
            std::mem::size_of::<FILE_ID_INFO>() as u32,
        )
    } == 0
    {
        return Err(std::io::Error::last_os_error());
    }
    let info = unsafe { info.assume_init() };
    Ok(Identity(
        info.VolumeSerialNumber,
        u128::from_le_bytes(info.FileId.Identifier),
        file.metadata()?.creation_time(),
    ))
}

pub fn open_log(path: &Path) -> std::io::Result<File> {
    #[cfg(unix)]
    let file: File = rustix::fs::open(
        path,
        rustix::fs::OFlags::RDONLY
            | rustix::fs::OFlags::NOFOLLOW
            | rustix::fs::OFlags::NONBLOCK
            | rustix::fs::OFlags::CLOEXEC,
        rustix::fs::Mode::empty(),
    )
    .map(File::from)
    .map_err(std::io::Error::from)?;
    #[cfg(windows)]
    let file = {
        use std::os::windows::fs::{MetadataExt, OpenOptionsExt};
        use windows_sys::Win32::Storage::FileSystem::{
            FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_OPEN_REPARSE_POINT,
        };
        let file = fs::OpenOptions::new()
            .read(true)
            .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
            .open(path)?;
        if file.metadata()?.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "Log files must not be reparse points",
            ));
        }
        file
    };
    if !file.metadata()?.is_file() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "Log files must be regular files",
        ));
    }
    Ok(file)
}

pub struct OpenedLog {
    pub path: PathBuf,
    pub identity: Identity,
    pub source: Source,
    pub modified: Option<SystemTime>,
    pub file: File,
}

pub enum ScanError {
    Rotated,
    Failed(String),
}

impl From<std::io::Error> for ScanError {
    fn from(error: std::io::Error) -> Self {
        if error.kind() == std::io::ErrorKind::NotFound {
            Self::Rotated
        } else {
            Self::Failed(format!("Read retained logs: {error}"))
        }
    }
}

pub fn scan(
    directory: &Path,
    before_open: &mut impl FnMut(&Path),
) -> Result<Vec<OpenedLog>, ScanError> {
    let directory = match fs::read_dir(directory) {
        Ok(directory) => directory,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(error.into()),
    };
    let mut files = Vec::new();
    for item in directory {
        let item = item?;
        let Some(source) = log_source(&item.file_name().to_string_lossy()) else {
            continue;
        };
        if !item.file_type()?.is_file() {
            continue;
        }
        let path = item.path();
        before_open(&path);
        let file = open_log(&path)?;
        let identity = identity(&file)?;
        let modified = file.metadata()?.modified().ok();
        files.push(OpenedLog {
            path,
            identity,
            source,
            modified,
            file,
        });
        if files.len() > 32 {
            return Err(ScanError::Failed(
                "Too many retained log files (maximum 32)".into(),
            ));
        }
    }
    files.sort_by(|a, b| (&a.modified, &a.path).cmp(&(&b.modified, &b.path)));
    Ok(files)
}
