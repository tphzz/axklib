use std::fmt::Write as _;
use std::fs::File;
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

#[cfg(unix)]
fn create_private_directory(path: &Path) -> std::io::Result<()> {
    use std::os::unix::fs::DirBuilderExt;
    let mut builder = std::fs::DirBuilder::new();
    builder.mode(0o700).create(path)
}

#[cfg(windows)]
fn create_private_directory(path: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use std::ptr::{null, null_mut};
    use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_ALL, LocalFree};
    use windows_sys::Win32::Security::Authorization::{
        EXPLICIT_ACCESS_W, SET_ACCESS, SetEntriesInAclW, TRUSTEE_IS_SID, TRUSTEE_IS_USER, TRUSTEE_W,
    };
    use windows_sys::Win32::Security::{
        GetTokenInformation, InitializeSecurityDescriptor, NO_INHERITANCE, SE_DACL_PROTECTED,
        SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR, SetSecurityDescriptorControl,
        SetSecurityDescriptorDacl, TOKEN_QUERY, TOKEN_USER, TokenUser,
    };
    use windows_sys::Win32::Storage::FileSystem::CreateDirectoryW;
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

    let mut token = null_mut();
    unsafe {
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    let result = (|| {
        let mut token_size = 0_u32;
        unsafe {
            let _ = GetTokenInformation(token, TokenUser, null_mut(), 0, &mut token_size);
        }
        if token_size == 0 {
            return Err(std::io::Error::last_os_error());
        }
        let word_size = std::mem::size_of::<usize>();
        let mut token_storage = vec![0_usize; (token_size as usize).div_ceil(word_size)];
        if unsafe {
            GetTokenInformation(
                token,
                TokenUser,
                token_storage.as_mut_ptr().cast(),
                token_size,
                &mut token_size,
            )
        } == 0
        {
            return Err(std::io::Error::last_os_error());
        }
        let token_user = unsafe { &*token_storage.as_ptr().cast::<TOKEN_USER>() };
        let access = EXPLICIT_ACCESS_W {
            grfAccessPermissions: GENERIC_ALL,
            grfAccessMode: SET_ACCESS,
            grfInheritance: NO_INHERITANCE,
            Trustee: TRUSTEE_W {
                pMultipleTrustee: null_mut(),
                MultipleTrusteeOperation: 0,
                TrusteeForm: TRUSTEE_IS_SID,
                TrusteeType: TRUSTEE_IS_USER,
                ptstrName: token_user.User.Sid.cast(),
            },
        };
        let mut acl = null_mut();
        let acl_status = unsafe { SetEntriesInAclW(1, &access, null(), &mut acl) };
        if acl_status != 0 {
            return Err(std::io::Error::from_raw_os_error(acl_status as i32));
        }
        let mut descriptor = SECURITY_DESCRIPTOR::default();
        let descriptor_ready = unsafe {
            InitializeSecurityDescriptor((&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(), 1)
                != 0
                && SetSecurityDescriptorDacl(
                    (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
                    1,
                    acl,
                    0,
                ) != 0
                && SetSecurityDescriptorControl(
                    (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
                    SE_DACL_PROTECTED,
                    SE_DACL_PROTECTED,
                ) != 0
        };
        if !descriptor_ready {
            unsafe {
                LocalFree(acl.cast());
            }
            return Err(std::io::Error::last_os_error());
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast(),
            bInheritHandle: 0,
        };
        let wide = path
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect::<Vec<_>>();
        let created = unsafe { CreateDirectoryW(wide.as_ptr(), &attributes) };
        let error = if created == 0 {
            Some(std::io::Error::last_os_error())
        } else {
            None
        };
        unsafe {
            LocalFree(acl.cast());
        }
        match error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    })();
    unsafe {
        CloseHandle(token);
    }
    result
}

fn validate_private_directory(path: &Path) -> Result<(), String> {
    let metadata = std::fs::symlink_metadata(path)
        .map_err(|error| format!("inspect sidecar runtime directory: {error}"))?;
    if !metadata.is_dir() {
        return Err("sidecar runtime path is not a directory".to_owned());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if metadata.uid() != rustix::process::geteuid().as_raw() {
            return Err("sidecar runtime directory is not owned by the current user".to_owned());
        }
        if metadata.permissions().mode() & 0o077 != 0 {
            return Err("sidecar runtime directory permissions are not owner-only".to_owned());
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_ATTRIBUTE_REPARSE_POINT;
        if metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err("sidecar runtime directory must not be a reparse point".to_owned());
        }
    }
    Ok(())
}
