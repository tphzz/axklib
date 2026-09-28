use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use super::records::Entry;

pub fn normalize_destination(mut path: PathBuf, log_directory: &Path) -> Result<PathBuf, String> {
    match path.extension().and_then(|part| part.to_str()) {
        None => {
            path.set_extension("log");
        }
        Some(extension) if extension.eq_ignore_ascii_case("log") => {}
        Some(_) => return Err("The log export filename must end in .log".into()),
    }
    let parent = path
        .parent()
        .ok_or("The log export has no parent directory")?;
    let parent = parent
        .canonicalize()
        .map_err(|e| format!("Resolve export directory: {e}"))?;
    if log_directory
        .canonicalize()
        .is_ok_and(|logs| parent.starts_with(logs))
    {
        return Err(
            "Choose an export destination outside the application's active log directory".into(),
        );
    }
    Ok(parent.join(path.file_name().ok_or("The log export has no filename")?))
}

fn temporary_sibling(destination: &Path) -> Result<(PathBuf, File), String> {
    let parent = destination
        .parent()
        .ok_or("The log export has no parent directory")?;
    for _ in 0..16 {
        let mut nonce = [0_u8; 16];
        getrandom::fill(&mut nonce).map_err(|e| format!("Reserve log export: {e}"))?;
        let nonce = format!("{:032x}", u128::from_ne_bytes(nonce));
        let path = parent.join(format!(".axkdeck-log-{nonce}.tmp"));
        match OpenOptions::new().create_new(true).write(true).open(&path) {
            Ok(file) => return Ok((path, file)),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(format!("Create log export: {error}")),
        }
    }
    Err("Could not reserve a log export file".into())
}

pub fn write_snapshot(
    destination: &Path,
    entries: &[Arc<Entry>],
) -> Result<Option<String>, String> {
    let (temporary, file) = temporary_sibling(destination)?;
    let result = (|| {
        let mut output = BufWriter::new(file);
        for entry in entries {
            writeln!(output, "[{}] {}", entry.source.label(), entry.text)
                .map_err(|e| format!("Write log export: {e}"))?;
        }
        output
            .flush()
            .map_err(|e| format!("Flush log export: {e}"))?;
        output
            .get_ref()
            .sync_all()
            .map_err(|e| format!("Synchronize log export: {e}"))?;
        drop(output);
        crate::file_publication::publish_file(&temporary, destination)
            .map(|outcome| outcome.warning)
    })();
    if result.is_err() {
        let _ = std::fs::remove_file(temporary);
    }
    result
}
