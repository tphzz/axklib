use super::{DesktopPreferencesStore, InterfaceScaleMode};
use crate::a_series_preferences::ASeriesGeneration;
use std::fs;

#[test]
fn a_series_preference_survives_reload_without_changing_other_preferences() {
    let root = super::tests::temporary_directory("a-series");
    let path = root.join("settings.json");
    let mut store = DesktopPreferencesStore::load(path.clone()).unwrap();
    assert_eq!(
        store.preferred_a_series_generation().unwrap(),
        ASeriesGeneration::A3000
    );
    store
        .set_interface_scale_mode(InterfaceScaleMode::OnePointFive)
        .unwrap();
    store.remember_package_export_directory(&root).unwrap();
    store
        .set_preferred_a_series_generation(ASeriesGeneration::A4000A5000)
        .unwrap();
    let mut reloaded = DesktopPreferencesStore::load(path.clone()).unwrap();
    assert_eq!(
        reloaded.preferred_a_series_generation().unwrap(),
        ASeriesGeneration::A4000A5000
    );
    assert_eq!(
        reloaded.interface_scale_mode(),
        InterfaceScaleMode::OnePointFive
    );
    assert_eq!(
        reloaded.package_export_directory(),
        Some(root.canonicalize().unwrap())
    );
    let json: serde_json::Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    assert_eq!(json["preferredASeriesGeneration"], "A4000_A5000");
    reloaded
        .set_interface_scale_mode(InterfaceScaleMode::Auto)
        .unwrap();
    assert_eq!(
        DesktopPreferencesStore::load(path)
            .unwrap()
            .preferred_a_series_generation()
            .unwrap(),
        ASeriesGeneration::A4000A5000
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn missing_a_series_preference_defaults_without_resetting_existing_settings() {
    let root = super::tests::temporary_directory("a-series-default");
    let path = root.join("settings.json");
    fs::write(&path, br#"{"schemaVersion":1,"appearance":{"interfaceScaleMode":"1.25"},"lastUsedDirectories":{}}"#).unwrap();
    let store = DesktopPreferencesStore::load(path).unwrap();
    assert_eq!(
        store.preferred_a_series_generation().unwrap(),
        ASeriesGeneration::A3000
    );
    assert_eq!(
        store.interface_scale_mode(),
        InterfaceScaleMode::OnePointTwentyFive
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn failed_save_keeps_the_previously_saved_generation() {
    let root = super::tests::temporary_directory("a-series-failure");
    let path = root.join("settings.json");
    let mut store = DesktopPreferencesStore::load(path.clone()).unwrap();
    fs::create_dir(&path).unwrap();
    assert!(
        store
            .set_preferred_a_series_generation(ASeriesGeneration::A4000A5000)
            .is_err()
    );
    assert_eq!(
        store.preferred_a_series_generation().unwrap(),
        ASeriesGeneration::A3000
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn unreadable_settings_are_not_overwritten_from_fallback_state() {
    let root = super::tests::temporary_directory("a-series-invalid");
    let path = root.join("settings.json");
    let invalid = br#"{"schemaVersion":1,"appearance":{"interfaceScaleMode":"auto"},"lastUsedDirectories":{},"preferredASeriesGeneration":"ROLAND"}"#;
    fs::write(&path, invalid).unwrap();
    let error = DesktopPreferencesStore::load(path.clone()).err().unwrap();
    let mut store = DesktopPreferencesStore::unavailable(path.clone(), error);
    assert!(store.preferred_a_series_generation().is_err());
    assert!(
        store
            .set_preferred_a_series_generation(ASeriesGeneration::A3000)
            .is_err()
    );
    assert!(
        store
            .set_interface_scale_mode(InterfaceScaleMode::One)
            .is_err()
    );
    assert_eq!(fs::read(&path).unwrap(), invalid);
    fs::remove_dir_all(root).unwrap();
}
