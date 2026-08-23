use std::path::{Path, PathBuf};

const VENDOR_DIRECTORY: &str = "tphzz";
const PRODUCT_DIRECTORY: &str = "axkdeck";

pub struct SettingsPaths {
    pub axkdeck_settings: PathBuf,
    pub sidecar_workspace_store: PathBuf,
}

impl SettingsPaths {
    pub fn from_config_directory(config_directory: &Path) -> Self {
        let root = config_directory
            .join(VENDOR_DIRECTORY)
            .join(PRODUCT_DIRECTORY);
        Self {
            axkdeck_settings: root.join("settings.json"),
            sidecar_workspace_store: root.join("axklib-server").join("workspaces.json"),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use super::SettingsPaths;

    #[test]
    fn settings_files_separate_axkdeck_from_its_sidecar_store() {
        let paths = SettingsPaths::from_config_directory(Path::new("config"));

        assert_eq!(
            paths.axkdeck_settings,
            Path::new("config/tphzz/axkdeck/settings.json")
        );
        assert_eq!(
            paths.sidecar_workspace_store,
            Path::new("config/tphzz/axkdeck/axklib-server/workspaces.json")
        );
    }
}
