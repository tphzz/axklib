use crate::diagnostic_format::{invalid_log_record, server_level};
use crate::diagnostic_level::Level;
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Source {
    Application,
    LocalServer,
}

impl Source {
    pub fn label(self) -> &'static str {
        match self {
            Self::Application => "Application",
            Self::LocalServer => "Local server",
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Subsystem {
    Application,
    Frontend,
    Http,
    Jobs,
    Filesystem,
    Objects,
    Devices,
    Audio,
    Other,
}

impl Subsystem {
    pub const ALL: [Self; 9] = [
        Self::Application,
        Self::Frontend,
        Self::Http,
        Self::Jobs,
        Self::Filesystem,
        Self::Objects,
        Self::Devices,
        Self::Audio,
        Self::Other,
    ];
    pub fn label(self) -> &'static str {
        match self {
            Self::Application => "Application",
            Self::Frontend => "Frontend",
            Self::Http => "HTTP",
            Self::Jobs => "Jobs",
            Self::Filesystem => "Filesystem",
            Self::Objects => "Objects",
            Self::Devices => "Devices",
            Self::Audio => "Audio",
            Self::Other => "Other",
        }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    pub id: u64,
    pub origin: Source,
    pub source: Subsystem,
    pub timestamp: Option<i64>,
    pub level: Level,
    pub text: String,
}

pub fn parse_entry(id: u64, source: Source, text: String) -> Entry {
    let (timestamp, mut level) = match source {
        Source::Application => application_header(&text).unwrap_or((None, None)),
        Source::LocalServer => server_header(&text).unwrap_or((None, None)),
    };
    let body = match source {
        Source::Application => {
            let mut rest = text.as_str();
            for _ in 0..4 {
                rest = bracket(rest).map_or("", |(_, rest)| rest);
            }
            rest.trim_start()
        }
        Source::LocalServer => text
            .split_once(' ')
            .and_then(|(_, rest)| bracket(rest))
            .map_or("", |(_, body)| body.trim_start()),
    };
    let structured = serde_json::from_str::<serde_json::Value>(body).ok();
    if source == Source::Application {
        if let Some(declared) = structured.as_ref().and_then(|value| value.get("level")) {
            if declared.as_str().and_then(Level::declared) != level {
                level = None;
            }
        }
    }
    let Some(level) = level else {
        return Entry {
            id,
            origin: source,
            source: Subsystem::Other,
            timestamp,
            level: Level::Error,
            text: invalid_log_record(&text, "missing, invalid or conflicting declared level"),
        };
    };
    let subsystem = structured
        .as_ref()
        .and_then(|value| value.get("source"))
        .and_then(|value| serde_json::from_value(value.clone()).ok())
        .unwrap_or(match source {
            Source::Application => Subsystem::Application,
            Source::LocalServer => Subsystem::Other,
        });
    Entry {
        id,
        origin: source,
        source: subsystem,
        timestamp,
        level,
        text,
    }
}

fn bracket(text: &str) -> Option<(&str, &str)> {
    text.strip_prefix('[')?.split_once(']')
}

fn application_header(text: &str) -> Option<(Option<i64>, Option<Level>)> {
    let (date, rest) = bracket(text)?;
    let (clock, rest) = bracket(rest)?;
    let (_, rest) = bracket(rest)?;
    let (severity, _) = bracket(rest)?;
    let format = time::macros::format_description!("[year]-[month]-[day] [hour]:[minute]:[second]");
    let timestamp = time::PrimitiveDateTime::parse(&format!("{date} {clock}"), &format)
        .ok()
        .map(|value| value.assume_utc().unix_timestamp() * 1000);
    Some((timestamp, Level::header(severity)))
}

fn server_header(text: &str) -> Option<(Option<i64>, Option<Level>)> {
    let (millis, rest) = text.split_once(' ')?;
    let timestamp = millis.parse::<i64>().ok()?;
    let (stream, body) = bracket(rest)?;
    if stream != "stdout" && stream != "stderr" {
        return None;
    }
    let body = body.trim_start();
    let severity = server_level(body).ok();
    Some((Some(timestamp), severity))
}

pub fn starts_record(source: Source, text: &str) -> bool {
    match source {
        Source::Application => application_header(text).is_some(),
        Source::LocalServer => server_header(text).is_some(),
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Filter {
    pub sources: Vec<Subsystem>,
    pub level: Level,
    pub search: String,
    pub since: Option<u64>,
}

impl Default for Filter {
    fn default() -> Self {
        Self {
            sources: Subsystem::ALL.to_vec(),
            level: Level::Info,
            search: String::new(),
            since: None,
        }
    }
}

impl Filter {
    pub fn all() -> Self {
        Self {
            level: Level::Trace,
            ..Self::default()
        }
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.search.len() > 1024 {
            return Err("Log search is limited to 1024 bytes".into());
        }
        Ok(())
    }

    pub fn matches(&self, entry: &Entry) -> bool {
        self.sources.contains(&entry.source)
            && self.since.is_none_or(|since| entry.id > since)
            && entry.level <= self.level
            && (self.search.is_empty()
                || entry
                    .text
                    .to_lowercase()
                    .contains(&self.search.to_lowercase()))
    }
}
