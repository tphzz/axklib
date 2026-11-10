use super::{disk_source, main_window, scoped_source, siblings};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axkdeck-companions-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path).unwrap();
        // System temporary directories can have symlinked ancestors.
        Self(path.canonicalize().unwrap())
    }
    fn file(&self, name: &str, size: u64) -> PathBuf {
        let path = self.0.join(name);
        std::fs::File::create(&path).unwrap().set_len(size).unwrap();
        path
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[test]
fn rejects_auxiliary_windows_and_unauthorized_seed_before_io() {
    assert!(main_window("main").is_ok());
    for label in ["logs", "mapping-program", "allocation-inspector", ""] {
        assert!(main_window(label).is_err());
    }
    assert!(
        scoped_source(Path::new("does-not-exist.img"), || false)
            .unwrap_err()
            .contains("not allowed")
    );
}

#[test]
fn enumerates_only_regular_same_folder_disks_and_deduplicates_file_identity() {
    let fixture = Fixture::new();
    let seed = fixture.file("first.img", 1);
    let second = fixture.file("second.IMA", 2);
    fixture.file("notes.txt", 3);
    std::fs::create_dir(fixture.0.join("folder.img")).unwrap();
    std::fs::hard_link(&second, fixture.0.join("alias.img")).unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(&second, fixture.0.join("link.img")).unwrap();
    let result = siblings(&seed, 1024, || true).unwrap();
    assert_eq!(result.len(), 1);
    assert_eq!(result[0].identity, disk_source(&second).unwrap().identity);
    assert_ne!(result[0].identity, disk_source(&seed).unwrap().identity);
    assert!(siblings(&seed, 2, || true).unwrap_err().contains("entries"));
}

#[test]
fn rejects_oversized_sources_lost_authority_and_candidate_byte_limits() {
    let fixture = Fixture::new();
    let seed = fixture.file("seed.img", 1);
    let too_large = fixture.file("large.img", 4 * 1024 * 1024 + 1);
    assert!(disk_source(&too_large).unwrap_err().contains("4 MiB"));
    let checks = std::cell::Cell::new(0);
    assert!(
        scoped_source(&seed, || {
            checks.set(checks.get() + 1);
            checks.get() == 1
        })
        .unwrap_err()
        .contains("changed")
    );
    for index in 0..17 {
        fixture.file(&format!("candidate-{index}.img"), 4 * 1024 * 1024);
    }
    assert!(
        siblings(&seed, 1024, || true)
            .unwrap_err()
            .contains("64 MiB")
    );
}

#[cfg(unix)]
#[test]
fn rejects_symlinked_seed_or_parent() {
    let fixture = Fixture::new();
    let seed = fixture.file("seed.img", 1);
    assert!(disk_source(&seed).is_ok());
    let link = fixture.0.join("seed-link.img");
    std::os::unix::fs::symlink(&seed, &link).unwrap();
    assert!(disk_source(&link).unwrap_err().contains("links"));
    let parent_link = fixture.0.join("parent-link");
    std::os::unix::fs::symlink(&fixture.0, &parent_link).unwrap();
    assert!(
        disk_source(&parent_link.join("seed.img"))
            .unwrap_err()
            .contains("links")
    );
}
