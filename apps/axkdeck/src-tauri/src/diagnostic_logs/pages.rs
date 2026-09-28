use serde::{Deserialize, Serialize};

use super::reader::LogReader;
use super::records::{Entry, Filter};

const PAGE_ENTRIES: usize = 400;
const PAGE_BYTES: usize = 1024 * 1024;

#[derive(Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct ReadRequest {
    pub filter: Filter,
    pub before: Option<u64>,
    pub after: Option<u64>,
    pub known_through: u64,
    pub metadata_only: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Page {
    pub entries: Vec<Entry>,
    pub older_cursor: Option<u64>,
    pub newer_cursor: Option<u64>,
    pub total: usize,
    pub new_count: usize,
    pub sequence: u64,
    pub history_changes: u64,
}

pub fn read_page(reader: &LogReader, request: &ReadRequest) -> Result<Page, String> {
    request.filter.validate()?;
    if request.before.is_some() && request.after.is_some() {
        return Err("Choose one log paging direction".into());
    }
    let matching = reader.matching(&request.filter);
    let total = matching.len();
    let mut page = Page {
        entries: Vec::new(),
        older_cursor: None,
        newer_cursor: None,
        total,
        new_count: matching
            .iter()
            .filter(|entry| entry.id > request.known_through)
            .count(),
        sequence: reader.sequence,
        history_changes: reader.history_changes,
    };
    if request.metadata_only {
        return Ok(page);
    }
    let position = |id| {
        matching
            .iter()
            .position(|entry| entry.id == id)
            .ok_or_else(|| {
                "Displayed log history has expired. Retry to load the latest entries.".to_string()
            })
    };
    let (start, end) = if let Some(after) = request.after {
        let start = position(after)? + 1;
        (start, (start + PAGE_ENTRIES).min(total))
    } else {
        let end = request.before.map(position).transpose()?.unwrap_or(total);
        (end.saturating_sub(PAGE_ENTRIES), end)
    };
    let mut bytes = 0;
    let indices: Box<dyn Iterator<Item = usize>> = if request.after.is_some() {
        Box::new(start..end)
    } else {
        Box::new((start..end).rev())
    };
    let mut included = Vec::new();
    for index in indices {
        bytes += matching[index].text.len();
        if bytes > PAGE_BYTES {
            break;
        }
        included.push(index);
    }
    included.sort_unstable();
    if let (Some(first), Some(last)) = (included.first(), included.last()) {
        if *first > 0 {
            page.older_cursor = Some(matching[*first].id);
        }
        if *last + 1 < total {
            page.newer_cursor = Some(matching[*last].id);
        }
    }
    page.entries = included
        .iter()
        .map(|index| matching[*index].as_ref().clone())
        .collect();
    Ok(page)
}
