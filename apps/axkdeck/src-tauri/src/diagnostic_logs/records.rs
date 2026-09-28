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

#[derive(Clone, Copy, Debug, Deserialize, Eq, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Level {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    pub id: u64,
    pub source: Source,
    pub timestamp: Option<i64>,
    pub level: Option<Level>,
    pub text: String,
}

pub fn parse_entry(id: u64, source: Source, text: String) -> Entry {
    let (timestamp, level) = match source {
        Source::Application => application_header(&text).unwrap_or((None, None)),
        Source::LocalServer => server_header(&text).unwrap_or((None, None)),
    };
    Entry {
        id,
        source,
        timestamp,
        level,
        text,
    }
}

fn level(value: &str) -> Option<Level> {
    match value.trim().to_ascii_lowercase().as_str() {
        "trace" => Some(Level::Trace),
        "debug" => Some(Level::Debug),
        "info" => Some(Level::Info),
        "warn" | "warning" => Some(Level::Warning),
        "error" | "critical" => Some(Level::Error),
        _ => None,
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
    Some((timestamp, level(severity)))
}

fn server_header(text: &str) -> Option<(Option<i64>, Option<Level>)> {
    let (millis, rest) = text.split_once(' ')?;
    let timestamp = millis.parse::<i64>().ok()?;
    let (stream, body) = bracket(rest)?;
    if stream != "stdout" && stream != "stderr" {
        return None;
    }
    let body = body.trim_start();
    let severity = if let Some((_, rest)) = body.strip_prefix('(').and_then(|v| v.split_once(") "))
    {
        bracket(rest).and_then(|(value, _)| level(value))
    } else {
        serde_json::from_str::<serde_json::Value>(body)
            .ok()
            .and_then(|value| {
                value
                    .get("level")
                    .or_else(|| value.get("severity"))
                    .and_then(|v| v.as_str())
                    .and_then(level)
            })
    };
    Some((Some(timestamp), severity))
}

pub fn starts_record(source: Source, text: &str) -> bool {
    match source {
        Source::Application => application_header(text).is_some(),
        Source::LocalServer => server_header(text).is_some(),
    }
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Filter {
    pub source: Option<Source>,
    pub minimum_level: Option<Level>,
    pub search: String,
    pub include_unclassified: bool,
    pub since: Option<u64>,
}

impl Filter {
    pub fn all() -> Self {
        Self {
            include_unclassified: true,
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
        self.source.is_none_or(|source| source == entry.source)
            && self.since.is_none_or(|since| entry.id > since)
            && match entry.level {
                Some(level) => self.minimum_level.is_none_or(|minimum| level >= minimum),
                None => self.include_unclassified,
            }
            && (self.search.is_empty()
                || entry
                    .text
                    .to_lowercase()
                    .contains(&self.search.to_lowercase()))
    }
}
