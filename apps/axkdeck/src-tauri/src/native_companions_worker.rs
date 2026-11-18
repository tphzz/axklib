use super::{
    MAXIMUM_DIRECTORY_ENTRIES, NativeDiskSource, disk_source, fingerprint, siblings, snapshot,
};
use crate::server_sidecar::{server_binary, suppress_child_console};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::io::{Read, Write};
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Serialize)]
pub(crate) struct Discovery {
    pub(super) sources: Vec<NativeDiskSource>,
    ambiguous: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Inspected {
    adopted: Vec<usize>,
    ambiguous: Vec<String>,
}

fn inspect(
    files: &[NativeDiskSource],
    selected: usize,
    allowed: &(impl Fn() -> bool + Sync),
) -> Result<(Inspected, Vec<String>), String> {
    let names: Vec<_> = files
        .iter()
        .map(|file| {
            let name = Path::new(&file.path)
                .file_name()
                .and_then(|n| n.to_str())
                .ok_or("Invalid floppy filename")?;
            Ok::<_, String>(serde_json::json!({"name":name, "size":file.size}))
        })
        .collect::<Result<_, _>>()?;
    let mut command =
        Command::new(server_binary().ok_or("Local companion inspector is unavailable")?);
    suppress_child_console(&mut command);
    let mut child = command
        .arg("--inspect-native-companions")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| e.to_string())?;
    let mut input = child.stdin.take().expect("piped stdin");
    let output = child.stdout.take().expect("piped stdout");
    std::thread::scope(|scope| {
        let writer = scope.spawn(move || -> Result<Vec<String>, String> {
            writeln!(
                input,
                "{}",
                serde_json::json!({"selected":selected, "files":names})
            )
            .map_err(|e| e.to_string())?;
            let mut hashes = Vec::with_capacity(files.len());
            for (index, file) in files.iter().enumerate() {
                if !allowed() {
                    return Err("Native companion inspection is no longer authorized".into());
                }
                let bytes = snapshot(file)?;
                let hash = format!("{:x}", Sha256::digest(&bytes));
                if index < selected && file.sha256 != hash {
                    return Err("Selected disk changed since admission".into());
                }
                input.write_all(&bytes).map_err(|e| e.to_string())?;
                hashes.push(hash);
            }
            Ok(hashes)
        });
        let reader = scope.spawn(move || -> Result<Inspected, String> {
            let mut bytes = Vec::new();
            output
                .take(1024 * 1024 + 1)
                .read_to_end(&mut bytes)
                .map_err(|e| e.to_string())?;
            if bytes.len() > 1024 * 1024 {
                return Err("Native inspector response exceeds its limit".into());
            }
            serde_json::from_slice(&bytes).map_err(|e| e.to_string())
        });
        let deadline = Instant::now() + Duration::from_secs(30);
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break Ok(status),
                Ok(None) if Instant::now() < deadline && allowed() => {
                    std::thread::sleep(Duration::from_millis(20))
                }
                other => {
                    let _ = child.kill();
                    let _ = child.wait();
                    break Err(other.err().map_or_else(
                        || "Companion inspection timed out or authorization changed".to_owned(),
                        |e| e.to_string(),
                    ));
                }
            }
        }?;
        let hashes = writer
            .join()
            .map_err(|_| "Native snapshot worker failed")??;
        if !status.success() {
            return Err("Local companion inspection failed; add companion disks explicitly".into());
        }
        Ok((
            reader
                .join()
                .map_err(|_| "Native inspection reader failed")??,
            hashes,
        ))
    })
}

pub(super) fn resolve(
    selected: &[NativeDiskSource],
    excluded: Vec<String>,
    allowed: impl Fn() -> bool + Sync,
) -> Result<Discovery, String> {
    if selected.is_empty()
        || selected.len() > 32
        || excluded.len() > MAXIMUM_DIRECTORY_ENTRIES
        || !allowed()
    {
        return Err("Invalid or unauthorized companion selection".into());
    }
    let mut seen = HashSet::new();
    for source in selected {
        let current = disk_source(Path::new(&source.path))?;
        if current.identity != source.identity
            || current.size != source.size
            || !seen.insert(source.identity.clone())
        {
            return Err("Selected disk identity changed or occurs twice".into());
        }
    }
    seen.extend(excluded);
    let mut files = selected.to_vec();
    files.extend(
        siblings(
            Path::new(&selected[0].path),
            MAXIMUM_DIRECTORY_ENTRIES,
            &allowed,
        )?
        .into_iter()
        .filter(|file| !seen.contains(&file.identity)),
    );
    let (result, hashes) = inspect(&files, selected.len(), &allowed)?;
    if result.adopted.len() > 32 - selected.len() {
        return Err("Companion set exceeds 32 disks".into());
    }
    let mut sources = Vec::new();
    for index in result.adopted {
        let inspected = files
            .get(index)
            .filter(|file| seen.insert(file.identity.clone()))
            .ok_or("Invalid companion result")?;
        let current = fingerprint(disk_source(Path::new(&inspected.path))?)?;
        if current.identity != inspected.identity || current.sha256 != hashes[index] {
            return Err("Companion changed after inspection".into());
        }
        sources.push(current);
    }
    if !allowed() {
        return Err("Native companion access changed".into());
    }
    Ok(Discovery {
        sources,
        ambiguous: result.ambiguous,
    })
}
