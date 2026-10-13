use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "lowercase")]
#[repr(u8)]
pub enum Level {
    Error = 1,
    Warning = 2,
    #[default]
    Info = 3,
    Debug = 4,
    Trace = 5,
}

impl Level {
    pub fn filter(self) -> log::LevelFilter {
        match self {
            Self::Error => log::LevelFilter::Error,
            Self::Warning => log::LevelFilter::Warn,
            Self::Info => log::LevelFilter::Info,
            Self::Debug => log::LevelFilter::Debug,
            Self::Trace => log::LevelFilter::Trace,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Error => "error",
            Self::Warning => "warning",
            Self::Info => "info",
            Self::Debug => "debug",
            Self::Trace => "trace",
        }
    }
    pub fn declared(value: &str) -> Option<Self> {
        match value {
            "error" => Some(Self::Error),
            "warning" => Some(Self::Warning),
            "info" => Some(Self::Info),
            "debug" => Some(Self::Debug),
            "trace" => Some(Self::Trace),
            _ => None,
        }
    }
    pub fn header(value: &str) -> Option<Self> {
        match value.trim().to_ascii_uppercase().as_str() {
            "ERROR" | "CRITICAL" => Some(Self::Error),
            "WARN" | "WARNING" => Some(Self::Warning),
            "INFO" => Some(Self::Info),
            "DEBUG" => Some(Self::Debug),
            "TRACE" => Some(Self::Trace),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_threshold_is_cumulative_and_matches_native_logging() {
        let levels = [
            Level::Error,
            Level::Warning,
            Level::Info,
            Level::Debug,
            Level::Trace,
        ];
        for (index, limit) in levels.iter().enumerate() {
            assert_eq!(
                levels.iter().filter(|level| *level <= limit).count(),
                index + 1
            );
            assert_eq!(*limit as u8, limit.filter() as u8);
            assert_eq!(Level::declared(limit.name()), Some(*limit));
        }
        for value in ["warn", "critical", "DEBUG", "off", "unclassified", ""] {
            assert_eq!(Level::declared(value), None);
        }
    }
}
