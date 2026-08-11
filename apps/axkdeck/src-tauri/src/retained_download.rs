use std::time::Duration;

pub fn client() -> Result<reqwest::blocking::Client, String> {
    reqwest::blocking::Client::builder()
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(10 * 60))
        .build()
        .map_err(|error| format!("create retained download client: {error}"))
}
