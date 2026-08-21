import { invoke } from '@tauri-apps/api/core';

export interface DesktopBuildInfo {
    schemaVersion: number;
    semanticVersion: string;
    projectVersion: string;
    sourceIdentity: string;
    releaseTag: string;
    isRelease: boolean;
    webviewEngine: string;
    webviewVersion: string | null;
}

export type DesktopBuildInfoState =
    { status: 'loading' } | { status: 'ready'; buildInfo: DesktopBuildInfo } | { status: 'error' };

export function desktopBuildInfo(): Promise<DesktopBuildInfo> {
    return invoke<DesktopBuildInfo>('desktop_build_info');
}
