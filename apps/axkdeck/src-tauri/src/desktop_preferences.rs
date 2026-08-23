use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

const SETTINGS_SCHEMA_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum InterfaceScaleMode {
    #[default]
    #[serde(rename = "auto")]
    Auto,
    #[serde(rename = "1")]
    One,
    #[serde(rename = "1.15")]
    OnePointFifteen,
    #[serde(rename = "1.25")]
    OnePointTwentyFive,
    #[serde(rename = "1.5")]
    OnePointFive,
}

#[derive(Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct AppearanceSettings {
    interface_scale_mode: InterfaceScaleMode,
}

#[derive(Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct LastUsedDirectories {
    package_export: Option<PathBuf>,
    directory_export: Option<PathBuf>,
    media_export: Option<PathBuf>,
    allocation_export: Option<PathBuf>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct DesktopPreferences {
    schema_version: u32,
    appearance: AppearanceSettings,
    last_used_directories: LastUsedDirectories,
}

impl Default for DesktopPreferences {
    fn default() -> Self {
        Self {
            schema_version: SETTINGS_SCHEMA_VERSION,
            appearance: AppearanceSettings::default(),
            last_used_directories: LastUsedDirectories::default(),
        }
    }
}

pub struct DesktopPreferencesStore {
    document_path: PathBuf,
    preferences: DesktopPreferences,
}

impl DesktopPreferencesStore {
    pub fn load(document_path: PathBuf) -> Result<Self, String> {
        let preferences: DesktopPreferences = match std::fs::read(&document_path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map_err(|error| format!("parse axkdeck settings: {error}"))?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                DesktopPreferences::default()
            }
            Err(error) => return Err(format!("read axkdeck settings: {error}")),
        };
        if preferences.schema_version != SETTINGS_SCHEMA_VERSION {
            return Err(format!(
                "unsupported axkdeck settings schema version: {}",
                preferences.schema_version
            ));
        }
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

    pub fn interface_scale_mode(&self) -> InterfaceScaleMode {
        self.preferences.appearance.interface_scale_mode
    }

    pub fn set_interface_scale_mode(&mut self, mode: InterfaceScaleMode) -> Result<(), String> {
        self.preferences.appearance.interface_scale_mode = mode;
        self.persist()
    }

    pub fn package_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_used_directories
            .package_export
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
        self.preferences.last_used_directories.package_export = Some(directory);
        self.persist()
    }

    pub fn directory_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_used_directories
            .directory_export
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
        self.preferences.last_used_directories.directory_export = Some(directory);
        self.persist()
    }

    pub fn media_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_used_directories
            .media_export
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
        self.preferences.last_used_directories.media_export = Some(directory);
        self.persist()
    }

    pub fn allocation_export_directory(&self) -> Option<PathBuf> {
        self.preferences
            .last_used_directories
            .allocation_export
            .as_ref()
            .filter(|directory| directory.is_dir())
            .cloned()
    }

    pub fn remember_allocation_export_directory(&mut self, directory: &Path) -> Result<(), String> {
        let directory = directory
            .canonicalize()
            .map_err(|error| format!("resolve allocation export directory: {error}"))?;
        if !directory.is_dir() {
            return Err("the allocation export location is not a directory".to_owned());
        }
        self.preferences.last_used_directories.allocation_export = Some(directory);
        self.persist()
    }

    fn persist(&self) -> Result<(), String> {
        let mut encoded = serde_json::to_vec_pretty(&self.preferences)
            .map_err(|error| format!("encode axkdeck settings: {error}"))?;
        encoded.push(b'\n');
        write_atomically(&self.document_path, &encoded)
    }
}

fn temporary_sibling(destination: &Path) -> Result<(PathBuf, File), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "axkdeck settings path has no parent directory".to_owned())?;
    let name = destination
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("settings");
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
            Err(error) => return Err(format!("create axkdeck settings staging file: {error}")),
        }
    }
    Err("could not reserve an axkdeck settings staging file".to_owned())
}

fn write_atomically(destination: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "axkdeck settings path has no parent directory".to_owned())?;
    std::fs::create_dir_all(parent)
        .map_err(|error| format!("create axkdeck settings directory: {error}"))?;
    let (temporary, mut output) = temporary_sibling(destination)?;
    let result = (|| {
        output
            .write_all(bytes)
            .map_err(|error| format!("write axkdeck settings: {error}"))?;
        output
            .sync_all()
            .map_err(|error| format!("flush axkdeck settings: {error}"))?;
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

    use super::{DesktopPreferencesStore, InterfaceScaleMode};

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
        let document = root.join("settings.json");

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
    fn interface_scale_mode_defaults_to_auto_and_survives_reload() {
        let root = temporary_directory("interface-scale");
        let document = root.join("settings.json");
        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");

        assert_eq!(store.interface_scale_mode(), InterfaceScaleMode::Auto);
        store
            .set_interface_scale_mode(InterfaceScaleMode::OnePointFifteen)
            .expect("persist interface scale");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(
            reloaded.interface_scale_mode(),
            InterfaceScaleMode::OnePointFifteen
        );
        let persisted: serde_json::Value = serde_json::from_slice(
            &fs::read(root.join("settings.json")).expect("read persisted settings"),
        )
        .expect("parse persisted settings");
        assert_eq!(persisted["schemaVersion"], 1);
        assert_eq!(persisted["appearance"]["interfaceScaleMode"], "1.15");
        assert!(persisted["lastUsedDirectories"]["packageExport"].is_null());
        assert!(persisted["lastUsedDirectories"]["directoryExport"].is_null());
        assert!(persisted["lastUsedDirectories"]["mediaExport"].is_null());
        assert!(persisted["lastUsedDirectories"]["allocationExport"].is_null());
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn obsolete_unreleased_preferences_file_is_not_read() {
        let root = temporary_directory("obsolete-preferences");
        fs::write(
            root.join("desktop-preferences.json"),
            br#"{"interfaceScaleMode":"1.5"}"#,
        )
        .expect("write obsolete preferences");

        let store = DesktopPreferencesStore::load(root.join("settings.json"))
            .expect("load current settings");
        assert_eq!(store.interface_scale_mode(), InterfaceScaleMode::Auto);
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn unsupported_interface_scale_modes_are_rejected() {
        let root = temporary_directory("invalid-interface-scale");
        let document = root.join("settings.json");
        fs::write(
            &document,
            br#"{"schemaVersion":1,"appearance":{"interfaceScaleMode":"2"},"lastUsedDirectories":{}}"#,
        )
        .expect("write invalid preferences");

        assert!(DesktopPreferencesStore::load(document).is_err());
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn unsupported_settings_schema_versions_are_rejected() {
        let root = temporary_directory("unsupported-schema");
        let document = root.join("settings.json");
        fs::write(
            &document,
            br#"{"schemaVersion":2,"appearance":{"interfaceScaleMode":"auto"},"lastUsedDirectories":{}}"#,
        )
        .expect("write unsupported settings");

        assert!(DesktopPreferencesStore::load(document).is_err());
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn unknown_settings_fields_are_rejected() {
        let root = temporary_directory("unknown-field");
        let document = root.join("settings.json");
        fs::write(
            &document,
            br#"{"schemaVersion":1,"appearance":{"interfaceScaleMode":"auto"},"lastUsedDirectories":{},"legacy":true}"#,
        )
        .expect("write settings with unknown field");

        assert!(DesktopPreferencesStore::load(document).is_err());
        fs::remove_dir_all(root).expect("remove temporary directory");
    }

    #[test]
    fn directory_export_location_is_remembered_separately() {
        let root = temporary_directory("directory-reload");
        let package_directory = root.join("packages");
        let directory_export = root.join("directory-exports");
        fs::create_dir(&package_directory).expect("create package directory");
        fs::create_dir(&directory_export).expect("create directory export location");
        let document = root.join("settings.json");

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
    fn allocation_export_location_is_remembered_separately() {
        let root = temporary_directory("allocation-reload");
        let package_directory = root.join("packages");
        let allocation_export = root.join("allocation-exports");
        fs::create_dir(&package_directory).expect("create package directory");
        fs::create_dir(&allocation_export).expect("create allocation export location");
        let document = root.join("settings.json");

        let mut store =
            DesktopPreferencesStore::load(document.clone()).expect("load empty preferences");
        store
            .remember_package_export_directory(&package_directory)
            .expect("remember package directory");
        store
            .remember_allocation_export_directory(&allocation_export)
            .expect("remember allocation export location");

        let reloaded = DesktopPreferencesStore::load(document).expect("reload preferences");
        assert_eq!(
            reloaded.allocation_export_directory(),
            Some(
                allocation_export
                    .canonicalize()
                    .expect("canonical allocation export location")
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
        let document = root.join("settings.json");

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
        let document = root.join("settings.json");

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
        let document = root.join("settings.json");

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
        let document = root.join("settings.json");
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
