use crate::diagnostic_level::Level;
use std::io::{self, BufRead, BufReader, Read};

pub fn invalid_log_record(original: &str, reason: &str) -> String {
    let mut end = original.len().min(4096);
    while !original.is_char_boundary(end) {
        end -= 1;
    }
    serde_json::json!({
        "event": "invalid_log_record",
        "source": "other",
        "level": "error",
        "message": "Logging contract violation; original severity is not inferred",
        "reason": reason,
        "originalText": &original[..end],
        "truncated": end < original.len(),
    })
    .to_string()
}

pub fn server_level(text: &str) -> Result<Level, &'static str> {
    if let Some((clock, header)) = text
        .strip_prefix('(')
        .and_then(|value| value.split_once(") "))
    {
        let format =
            time::macros::format_description!("[year]-[month]-[day] [hour]:[minute]:[second]");
        if time::PrimitiveDateTime::parse(clock, &format).is_err() {
            return Err("invalid framework timestamp");
        }
        return header
            .strip_prefix('[')
            .and_then(|value| value.split_once(']'))
            .and_then(|(level, _)| Level::header(level))
            .ok_or("missing or invalid framework level");
    }
    let value: serde_json::Value =
        serde_json::from_str(text).map_err(|_| "unrecognized log format")?;
    value
        .get("level")
        .and_then(serde_json::Value::as_str)
        .and_then(Level::declared)
        .ok_or("missing or invalid declared level")
}

pub fn normalize_server_record(text: &str) -> (Level, String) {
    match server_level(text) {
        Ok(level) => (level, text.to_owned()),
        Err(reason) => (Level::Error, invalid_log_record(text, reason)),
    }
}

fn decode_record(bytes: &[u8], oversized: bool) -> (Level, String) {
    let text = String::from_utf8_lossy(bytes);
    if oversized {
        return (
            Level::Error,
            invalid_log_record(&text, "record exceeds 64 KiB"),
        );
    }
    if std::str::from_utf8(bytes).is_err() {
        return (Level::Error, invalid_log_record(&text, "invalid UTF-8"));
    }
    normalize_server_record(text.trim_end_matches('\r'))
}

pub fn read_server_records<R: Read>(
    reader: R,
    mut emit: impl FnMut(Level, &str) -> bool,
) -> io::Result<()> {
    let mut input = BufReader::new(reader);
    let mut pending = Vec::new();
    let mut oversized = false;
    loop {
        let (consumed, complete) = {
            let chunk = input.fill_buf()?;
            if chunk.is_empty() {
                break;
            }
            let end = chunk.iter().position(|byte| *byte == b'\n');
            let length = end.unwrap_or(chunk.len());
            let available = (64 * 1024usize).saturating_sub(pending.len());
            pending.extend_from_slice(&chunk[..length.min(available)]);
            oversized |= length > available;
            (length + usize::from(end.is_some()), end.is_some())
        };
        input.consume(consumed);
        if complete {
            let (level, text) = decode_record(&pending, oversized);
            if !emit(level, &text) {
                return Ok(());
            }
            pending.clear();
            oversized = false;
        }
    }
    if !pending.is_empty() || oversized {
        let (level, text) = decode_record(&pending, oversized);
        emit(level, &text);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_undeclared_severity_without_inspecting_message_words_or_streams() {
        for text in [
            "plain [ERROR] filename",
            "(filename) [INFO] not a framework header",
            r#"{"status":500}"#,
            r#"{"severity":"error"}"#,
            r#"{"level":"warn"}"#,
        ] {
            let (level, output) = normalize_server_record(text);
            assert_eq!(level, Level::Error);
            let value: serde_json::Value = serde_json::from_str(&output).unwrap();
            assert_eq!(value["event"], "invalid_log_record");
            assert_eq!(value["originalText"], text);
            assert_eq!(normalize_server_record(&output), (Level::Error, output));
        }
    }

    #[test]
    fn original_unicode_and_newlines_are_bounded_and_escaped() {
        let text = "\n\"".to_owned() + &"\u{1f600}".repeat(2000);
        let output = invalid_log_record(&text, "invalid");
        let value: serde_json::Value = serde_json::from_str(&output).unwrap();
        let original = value["originalText"].as_str().unwrap();
        assert!(text.starts_with(original));
        assert!(original.len() <= 4096);
        assert_eq!(value["truncated"], true);
        assert!(!output.contains('\n'));
    }

    #[test]
    fn streamed_records_recover_after_oversized_and_invalid_utf8_input() {
        let mut input = vec![b'x'; 1024 * 1024];
        input.extend_from_slice(b"\n\xff\n{\"level\":\"warning\"}");
        let mut records = Vec::new();
        read_server_records(input.as_slice(), |level, text| {
            records.push((level, text.to_owned()));
            true
        })
        .unwrap();
        assert_eq!(records.len(), 3);
        assert!(records[0].1.len() < 5000);
        assert!(records[0].1.contains("record exceeds 64 KiB"));
        assert!(records[1].1.contains("invalid UTF-8"));
        assert_eq!(
            records[2],
            (Level::Warning, r#"{"level":"warning"}"#.into())
        );
    }
}
