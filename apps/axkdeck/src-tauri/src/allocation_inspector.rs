use std::path::PathBuf;

use serde::Deserialize;
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW_LABEL: &str = "allocation-inspector";

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct AllocationInspectorRequest {
    image_id: String,
    revision: u64,
    partition_index: u32,
    partition_name: String,
}

fn inspector_path(request: &AllocationInspectorRequest) -> PathBuf {
    let query = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("view", "allocation")
        .append_pair("imageId", &request.image_id)
        .append_pair("revision", &request.revision.to_string())
        .append_pair("partitionIndex", &request.partition_index.to_string())
        .append_pair("partitionName", &request.partition_name)
        .finish();
    PathBuf::from(format!("index.html?{query}"))
}

#[tauri::command]
pub(crate) async fn open_allocation_inspector(
    app: AppHandle,
    request: AllocationInspectorRequest,
) -> Result<(), String> {
    if let Some(existing) = app.get_webview_window(WINDOW_LABEL) {
        existing.destroy().map_err(|error| error.to_string())?;
    }
    WebviewWindowBuilder::new(
        &app,
        WINDOW_LABEL,
        WebviewUrl::App(inspector_path(&request)),
    )
    .title(format!("{} allocation", request.partition_name))
    .inner_size(1280.0, 800.0)
    .min_inner_size(760.0, 520.0)
    .build()
    .map_err(|error| error.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{AllocationInspectorRequest, inspector_path};

    #[test]
    fn inspector_path_encodes_revision_pinned_partition_identity() {
        let path = inspector_path(&AllocationInspectorRequest {
            image_id: "image/one".to_string(),
            revision: 7,
            partition_index: 2,
            partition_name: "Sounds & Tests".to_string(),
        });
        assert_eq!(
            path.to_string_lossy(),
            "index.html?view=allocation&imageId=image%2Fone&revision=7&partitionIndex=2&partitionName=Sounds+%26+Tests"
        );
    }
}
