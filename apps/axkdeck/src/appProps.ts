import type { InterfaceScaleController } from './lib/interfaceScale';
import type { ASeriesPreferences } from './lib/aSeriesPreferences.svelte';

export interface AppProps {
    aSeriesPreferences?: ASeriesPreferences;
    interfaceScaling?: InterfaceScaleController | null;
    initialExperimentalWarningOpen?: boolean;
    openConnectionSettingsOnStart?: boolean;
}
