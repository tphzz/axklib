import type { InterfaceScaleController } from './lib/interfaceScale';

export interface AppProps {
    interfaceScaling?: InterfaceScaleController | null;
    initialExperimentalWarningOpen?: boolean;
    openConnectionSettingsOnStart?: boolean;
}
