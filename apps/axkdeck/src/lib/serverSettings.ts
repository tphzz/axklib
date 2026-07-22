import { invoke } from '@tauri-apps/api/core';

import { AxklibHttpApiClient, type AxklibApiConnection } from './httpApiClient';

export interface RemoteServerSettingsInput {
    baseUrl: string;
    bearerToken: string;
}

export interface RemoteServerSettingsView {
    mode: 'local' | 'remote';
    baseUrl: string | null;
    tokenConfigured: boolean;
    secureStorageError: string | null;
}

interface ValidatedRemoteConnection extends AxklibApiConnection {
    mode: 'remote';
}

export function remoteServerSettings(): Promise<RemoteServerSettingsView> {
    return invoke('remote_server_settings');
}

export async function configureRemoteServer(settings: RemoteServerSettingsInput): Promise<RemoteServerSettingsView> {
    const connection = await invoke<ValidatedRemoteConnection>('validate_remote_server_settings', { settings });
    await new AxklibHttpApiClient(connection).discover();
    return invoke('configure_remote_server', { settings });
}

export function useLocalServer(): Promise<RemoteServerSettingsView> {
    return invoke('use_local_server');
}
