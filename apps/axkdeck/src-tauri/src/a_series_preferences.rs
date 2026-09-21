use std::sync::Mutex;

use serde::{Deserialize, Serialize};
use tauri::State;

use crate::desktop_preferences::DesktopPreferencesStore;

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum ASeriesGeneration {
    #[default]
    A3000,
    #[serde(rename = "A4000_A5000")]
    A4000A5000,
}

#[tauri::command]
pub fn desktop_preferred_a_series_generation(
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<ASeriesGeneration, String> {
    preferences
        .lock()
        .map_err(|error| error.to_string())?
        .preferred_a_series_generation()
}

#[tauri::command]
pub fn set_desktop_preferred_a_series_generation(
    generation: ASeriesGeneration,
    preferences: State<'_, Mutex<DesktopPreferencesStore>>,
) -> Result<(), String> {
    preferences
        .lock()
        .map_err(|error| error.to_string())?
        .set_preferred_a_series_generation(generation)
}
