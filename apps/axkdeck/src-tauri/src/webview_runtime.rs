use tauri::WebviewWindow;

#[cfg(windows)]
pub const ENGINE: &str = "Microsoft Edge WebView2";

#[cfg(target_os = "macos")]
pub const ENGINE: &str = "Apple WebKit";

#[cfg(not(any(windows, target_os = "macos")))]
pub const ENGINE: &str = "WebKitGTK";

#[cfg(windows)]
pub async fn version(window: &WebviewWindow) -> Option<String> {
    let (sender, mut receiver) = tauri::async_runtime::channel(1);
    if let Err(error) = window.with_webview(move |webview| {
        let environment = webview.environment();
        let mut raw_version = Default::default();
        let result = unsafe { environment.BrowserVersionString(&mut raw_version) }
            .map(|()| webview2_com::take_pwstr(raw_version))
            .map_err(|error| error.to_string());
        let _ = sender.try_send(result);
    }) {
        log::warn!("query active WebView2 environment: {error}");
        return None;
    }

    match receiver.recv().await {
        Some(Ok(version)) if !version.trim().is_empty() => Some(version),
        Some(Ok(_)) => {
            log::warn!("active WebView2 environment returned an empty version");
            None
        }
        Some(Err(error)) => {
            log::warn!("query active WebView2 version: {error}");
            None
        }
        None => {
            log::warn!("active WebView2 version query did not return a result");
            None
        }
    }
}

#[cfg(not(windows))]
pub async fn version(_window: &WebviewWindow) -> Option<String> {
    match tauri::webview_version() {
        Ok(version) if !version.trim().is_empty() => Some(version),
        Ok(_) => {
            log::warn!("active web view engine returned an empty version");
            None
        }
        Err(error) => {
            log::warn!("query active web view version: {error}");
            None
        }
    }
}
