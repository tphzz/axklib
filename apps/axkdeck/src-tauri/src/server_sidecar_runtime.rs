use crate::private_directory::{create_private_directory, validate_private_directory};
use std::fmt::Write as _;
#[cfg(unix)]
use std::fs::File;
#[cfg(unix)]
use std::io::Read;
use std::path::{Path, PathBuf};

pub(super) struct PrivateRuntimeDirectory {
    path: PathBuf,
    #[cfg(unix)]
    handle: File,
}

impl PrivateRuntimeDirectory {
    pub(super) fn create(parent: &Path) -> Result<Self, String> {
        for _ in 0..32 {
            let mut random = [0_u8; 16];
            getrandom::fill(&mut random)
                .map_err(|error| format!("generate sidecar runtime directory name: {error}"))?;
            let mut name = String::with_capacity(random.len() * 2);
            for byte in random {
                write!(&mut name, "{byte:02x}").expect("writing to a String cannot fail");
            }
            let path = parent.join(format!("axkdeck-server-{name}"));
            match create_private_directory(&path) {
                Ok(()) => return Self::open(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => {
                    return Err(format!("create private sidecar runtime directory: {error}"));
                }
            }
        }
        Err("could not reserve a unique sidecar runtime directory".to_owned())
    }

    pub(super) fn open(path: PathBuf) -> Result<Self, String> {
        validate_private_directory(&path)?;
        #[cfg(unix)]
        let handle = {
            use rustix::fs::{Mode, OFlags, open};
            let descriptor = open(
                &path,
                OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|error| format!("retain sidecar runtime directory: {error}"))?;
            File::from(descriptor)
        };
        Ok(Self {
            path,
            #[cfg(unix)]
            handle,
        })
    }

    #[cfg(test)]
    pub(super) fn path(&self) -> &Path {
        &self.path
    }

    pub(super) fn connection_path(&self) -> PathBuf {
        self.path.join("connection.json")
    }

    pub(super) fn read_connection(&self) -> Result<Vec<u8>, String> {
        #[cfg(unix)]
        {
            use rustix::fs::{Mode, OFlags, openat};
            let descriptor = openat(
                &self.handle,
                "connection.json",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|error| format!("read sidecar connection file: {error}"))?;
            let mut file = File::from(descriptor);
            let metadata = file
                .metadata()
                .map_err(|error| format!("inspect sidecar connection file: {error}"))?;
            if !metadata.is_file() {
                return Err("sidecar connection entry is not a regular file".to_owned());
            }
            let mut document = Vec::new();
            file.read_to_end(&mut document)
                .map_err(|error| format!("read sidecar connection file: {error}"))?;
            Ok(document)
        }
        #[cfg(windows)]
        {
            validate_private_directory(&self.path)?;
            let path = self.connection_path();
            let metadata = std::fs::symlink_metadata(&path)
                .map_err(|error| format!("inspect sidecar connection file: {error}"))?;
            if !metadata.is_file() {
                return Err("sidecar connection entry is not a regular file".to_owned());
            }
            std::fs::read(path).map_err(|error| format!("read sidecar connection file: {error}"))
        }
    }

    pub(super) fn remove_connection(&self) -> Result<(), String> {
        #[cfg(unix)]
        {
            use rustix::fs::{AtFlags, unlinkat};
            unlinkat(&self.handle, "connection.json", AtFlags::empty())
                .map_err(|error| format!("remove consumed sidecar connection file: {error}"))
        }
        #[cfg(windows)]
        {
            validate_private_directory(&self.path)?;
            std::fs::remove_file(self.connection_path())
                .map_err(|error| format!("remove consumed sidecar connection file: {error}"))
        }
    }
}

impl Drop for PrivateRuntimeDirectory {
    fn drop(&mut self) {
        if validate_private_directory(&self.path).is_ok() {
            let _ = self.remove_connection();
            let _ = std::fs::remove_dir(&self.path);
        }
    }
}
