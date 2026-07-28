import {
    configureRemoteServer,
    remoteServerSettings,
    useLocalServer,
    type RemoteServerSettingsInput,
    type RemoteServerSettingsView,
} from '../../lib/serverSettings';

export interface ConnectionActions {
    load(): Promise<RemoteServerSettingsView>;
    saveRemote(input: RemoteServerSettingsInput): Promise<void>;
    useLocal(): Promise<void>;
}

export function createConnectionActions(reload: () => void = () => window.location.reload()): ConnectionActions {
    return {
        load: remoteServerSettings,
        async saveRemote(input) {
            await configureRemoteServer(input);
            reload();
        },
        async useLocal() {
            await useLocalServer();
            reload();
        },
    };
}
