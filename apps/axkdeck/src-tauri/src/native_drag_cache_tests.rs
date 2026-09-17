use crate::native_drag_cache::{DragCache, Limits};
use std::sync::atomic::{AtomicUsize, Ordering};

fn root() -> std::path::PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = std::env::temp_dir().join(format!(
        "axkdeck-drag-test-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir(&path).unwrap();
    path
}

#[test]
fn cache_accounts_across_instances_and_retains_handed_off_paths() {
    let parent = root();
    let path = parent.join("cache");
    let limits = Limits {
        bytes: 8192,
        entries: 2,
        ready_seconds: 10,
        retained_seconds: 100,
    };
    let mut first = DragCache::open(&path, limits).unwrap();
    let mut second = DragCache::open(&path, limits).unwrap();
    let ticket = first.reserve(2048, 100).unwrap();
    assert_eq!(ticket.id.len(), 32);
    assert!(
        ticket
            .id
            .bytes()
            .all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'))
    );
    first.begin(&ticket.id).unwrap();
    assert!(first.begin(&ticket.id).is_err());
    std::fs::create_dir(&ticket.destination).unwrap();
    std::fs::write(ticket.destination.join("RAW #1"), [1, 2, 3]).unwrap();
    first.finish(&ticket.id, true, 100).unwrap();
    let other = second.reserve(2048, 100).unwrap();
    second.begin(&other.id).unwrap();
    assert!(first.reserve(1024, 100).is_err());
    second.cancel(&other.id).unwrap();
    assert!(other.control.check().is_err());
    second.finish(&other.id, false, 100).unwrap();
    let paths = first.claim(&ticket.id, 101).unwrap();
    assert_eq!(paths, [ticket.destination.join("RAW #1")]);
    first.end(&ticket.id, true).unwrap();
    drop(first);
    second.prune(120).unwrap();
    assert_eq!(std::fs::read(&paths[0]).unwrap(), [1, 2, 3]);
    second.prune(202).unwrap();
    assert!(!paths[0].exists());
    drop(second);
    std::fs::remove_dir_all(parent).unwrap();
}

#[test]
fn abandoned_preparation_expires_but_live_workers_are_not_removed() {
    let parent = root();
    let path = parent.join("cache");
    let mut first = DragCache::open(&path, Limits::default()).unwrap();
    let ticket = first.reserve(1024, 1).unwrap();
    first.begin(&ticket.id).unwrap();
    let mut second = DragCache::open(&path, Limits::default()).unwrap();
    second.prune(100_000).unwrap();
    assert!(ticket.destination.parent().unwrap().exists());
    drop(first);
    second.prune(100_000).unwrap();
    assert!(!ticket.destination.parent().unwrap().exists());
    drop(second);
    std::fs::remove_dir_all(parent).unwrap();
}

#[test]
fn failed_or_cancelled_preparation_cannot_be_claimed() {
    let parent = root();
    let mut cache = DragCache::open(&parent.join("cache"), Limits::default()).unwrap();
    let ticket = cache.reserve(1024, 100).unwrap();
    cache.begin(&ticket.id).unwrap();
    assert!(cache.claim(&ticket.id, 100).is_err());
    cache.cancel(&ticket.id).unwrap();
    cache.finish(&ticket.id, true, 100).unwrap();
    assert!(!ticket.destination.parent().unwrap().exists());
    assert!(cache.claim(&ticket.id, 100).is_err());
    assert!(cache.reserve(u64::MAX, 100).is_err());
    assert!(cache.claim("../escape", 100).is_err());
    drop(cache);
    std::fs::remove_dir_all(parent).unwrap();
}
