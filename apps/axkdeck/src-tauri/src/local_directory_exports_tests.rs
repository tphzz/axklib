use super::*;

#[test]
fn cancellation_reaches_pending_and_active_saves_without_affecting_other_candidates() {
    let mut store = DirectorySaveCandidateStore::default();
    store.insert("first".into(), PathBuf::from("first"));
    store.insert("second".into(), PathBuf::from("second"));
    store.cancel("first");
    let (path, first) = store.begin("first").unwrap();
    assert_eq!(path, PathBuf::from("first"));
    assert_eq!(first.check().unwrap_err(), "Directory export cancelled");
    let (_, second) = store.begin("second").unwrap();
    assert!(second.check().is_ok());
    assert!(store.begin("second").is_err());
    store.cancel("second");
    assert!(second.check().is_err());
    store.finish("first");
    store.finish("second");
    store.cancel("second");
    store.cancel("unknown");
    assert!(store.values.is_empty());
}

#[test]
fn pending_destinations_expire_but_active_cancellation_handles_do_not() {
    let mut store = DirectorySaveCandidateStore::default();
    store.insert("expired".into(), PathBuf::from("expired"));
    store.insert("active".into(), PathBuf::from("active"));
    let (_, control) = store.begin("active").unwrap();
    for candidate in store.values.values_mut() {
        candidate.created = Instant::now() - Duration::from_secs(301);
    }
    assert!(store.begin("expired").is_err());
    store.insert("new".into(), PathBuf::from("new"));
    assert!(!store.values.contains_key("expired"));
    store.cancel("active");
    assert!(control.check().is_err());
    store.finish("active");
    assert_eq!(store.values.len(), 1);
}
