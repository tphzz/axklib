use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

#[derive(Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct DesktopPreferences {
    last_package_export_directory: Option<PathBuf>,
    last_directory_export_directory: Option<PathBuf>,
    last_media_export_directory: Option<PathBuf>,
}

pub struct DesktopPreferencesStore {
    document_path: PathBuf,
    preferences: DesktopPreferences,
}

impl DesktopPreferencesStore {
    pub fn load(document_path: PathBuf) -> Result<Self, String> {
        let preferences = match std::fs::read(&document_path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map_err(|error| format!("parse desktop preferences: {error}"))?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                DesktopPreferences::default()
            }
            Err(error) => return Err(format!("read desktop preferences: {error}")),
        };
        Ok(Self {
            document_path,
            preferences,
        })
    }

    pub fn empty(document_path: PathBuf) -> Self {
        Self {
            document_path,
            preferences: DesktopPreferences::default(),
        }
    }

    pub fn package_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_package_export_directory
            .as_ref()
            .filter(|directory| directory.is_dir())
            .cloned()
    }

    pub fn remember_package_export_directory(&mut self, directory: &Path) -> Result<(), String> {
        let directory = directory
            .canonicalize()
            .map_err(|error| format!("resolve package export directory: {error}"))?;
        if !directory.is_dir() {
            return Err("the package export location is not a directory".to_owned());
        }
        self.preferences.last_package_export_directory = Some(directory);
        self.persist()
    }

    pub fn directory_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_directory_export_directory
            .as_ref()
            .filter(|directory| directory.is_dir())
            .cloned()
    }

    pub fn remember_directory_export_directory(&mut self, directory: &Path) -> Result<(), String> {
        let directory = directory
            .canonicalize()
            .map_err(|error| format!("resolve directory export location: {error}"))?;
        if !directory.is_dir() {
            return Err("the directory export location is not a directory".to_owned());
        }
        self.preferences.last_directory_export_directory = Some(directory);
        self.persist()
    }

    pub fn media_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_media_export_directory
            .as_ref()
            .filter(|directory| directory.is_dir())
            .cloned()
    }

    pub fn remember_media_export_directory(&mut self, directory: &Path) -> Result<(), String> {
        let directory = directory
            .canonicalize()
            .map_err(|error| format!("resolve media export directory: {error}"))?;
        if !directory.is_dir() {
            return Err("the media export location is not a directory".to_owned());
        }
        self.preferences.last_media_export_directory = Some(directory);
        self.persist()
    }

    fn persist(&self) -> Result<(), String> {
        let mut encoded = serde_json::to_vec_pretty(&self.preferences)
            .map_err(|error| format!("encode desktop preferences: {error}"))?;
        encoded.push(b'\n');
        write_atomically(&self.document_path, &encoded)
    }
}

fn temporary_sibling(destination: &Path) -> Result<(PathBuf, File), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "desktop preferences path has no parent directory".to_owned())?;
    let name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("desktop-preferences");
    for attempt in 0_u8..16 {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| "system clock is unavailable".to_owned())?
            .as_nanos();
        let path = parent.join(format!(
            ".{name}.{}-{nonce}-{attempt}.tmp",
            std::process::id()
        ));
        match OpenOptions::new().create_new(true).write(true).open(&path) {
            Ok(file) => return Ok((path, file)),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(format!("create desktop preferences staging file: {error}")),
        }
    }
    Err("could not reserve a desktop preferences staging file".to_owned())
}

fn write_atomically(destination: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "desktop preferences path has no parent directory".to_owned())?;
    std::fs::create_dir_all(parent)
        .map_err(|error| format!("create desktop preferences directory: {error}"))?;
    let (temporary, mut output) = temporary_sibling(destination)?;
    let result = (|| {
        output
            .write_all(bytes)
            .map_err(|error| format!("write desktop preferences: {error}"))?;
        output
            .sync_all()
            .map_err(|error| format!("flush desktop preferences: {error}"))?;
        drop(output);
        let outcome = crate::file_publication::publish_file(&temporary, destination)?;
        if let Some(warning) = outcome.warning {
            log::warn!("{warning}");
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
    result
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::DesktopPreferencesStore;

    fn temporary_directory(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "axkdeck-preferences-{name}-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir_all(&directory).expect("create temporary directory");
        directory
    }

    #[test]
    fn package_export_directory_survives_store_reload() {
        let root = temporary_directory("reload");
        let export_directory = root.join("exports");
        fs::create_dir(&export_directory).expect("create export directory");
        let document = root.join("desktop-preferences.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&export_directory)
            .expect("remember export directory");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(
            reloaded.package_export_directory(),
            Some(
                export_directory
                    .canonicalize()
                    .expect("canonical export directory")
            )
        );
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn directory_export_location_is_remembered_separately() {
        let root = temporary_directory("directory-reload");
        let package_directory = root.join("packages");
        let directory_export = root.join("directory-exports");
        fs::create_dir(&package_directory).expect("create package directory");
        fs::create_dir(&directory_export).expect("create directory export location");
        let document = root.join("desktop-preferences.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&package_directory)
            .expect("remember package directory");
        store
            .remember_directory_export_directory(&directory_export)
            .expect("remember directory export location");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(
            reloaded.package_export_directory(),
            Some(
                package_directory
                    .canonicalize()
                    .expect("canonical package directory")
            )
        );
        assert_eq!(
            reloaded.directory_export_directory(),
            Some(
                directory_export
                    .canonicalize()
                    .expect("canonical directory export location")
            )
        );
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn newer_package_export_directory_replaces_the_previous_preference() {
        let root = temporary_directory("replace");
        let first_directory = root.join("first");
        let second_directory = root.join("second");
        fs::create_dir(&first_directory).expect("create first export directory");
        fs::create_dir(&second_directory).expect("create second export directory");
        let document = root.join("desktop-preferences.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&first_directory)
            .expect("remember first export directory");
        store
            .remember_package_export_directory(&second_directory)
            .expect("remember second export directory");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(
            reloaded.package_export_directory(),
            Some(
                second_directory
                    .canonicalize()
                    .expect("canonical second export directory")
            )
        );
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn unavailable_package_export_directory_is_ignored() {
        let root = temporary_directory("missing");
        let export_directory = root.join("exports");
        fs::create_dir(&export_directory).expect("create export directory");
        let document = root.join("desktop-preferences.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&export_directory)
            .expect("remember export directory");
        fs::remove_dir(&export_directory).expect("remove export directory");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(reloaded.package_export_directory(), None);
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn package_export_path_that_is_no_longer_a_directory_is_ignored() {
        let root = temporary_directory("not-directory");
        let export_directory = root.join("exports");
        fs::create_dir(&export_directory).expect("create export directory");
        let document = root.join("desktop-preferences.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&export_directory)
            .expect("remember export directory");
        fs::remove_dir(&export_directory).expect("remove export directory");
        fs::write(&export_directory, b"not a directory").expect("replace directory with file");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(reloaded.package_export_directory(), None);
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn malformed_preferences_are_rejected_for_startup_to_reset() {
        let root = temporary_directory("malformed");
        let document = root.join("desktop-preferences.json");
        fs::write(&document, b"{not-json").expect("write malformed preferences");

        assert!(DesktopPreferencesStore::load(document).is_err());
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn persistence_failure_keeps_the_session_directory_available() {
        let root = temporary_directory("write-failure");
        let export_directory = root.join("exports");
        fs::create_dir(&export_directory).expect("create export directory");
        let document = root.join("preferences-as-directory");
        fs::create_dir(&document).expect("create conflicting directory");

        let mut store = DesktopPreferencesStore::empty(document);
        assert!(
            store
                .remember_package_export_directory(&export_directory)
                .is_err()
        );
        assert_eq!(
            store.package_export_directory(),
            Some(
                export_directory
                    .canonicalize()
                    .expect("canonical export directory")
            )
        );
        fs::remove_dir_all(root).expect("remove temporary directory");
    }
}
