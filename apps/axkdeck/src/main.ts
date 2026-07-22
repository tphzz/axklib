import './app.css';
import { mount } from 'svelte';
import App from './App.svelte';
import { invoke } from '@tauri-apps/api/core';
import { installDiagnostics, reportError, reportInfo } from './lib/diagnostics';

const target = document.getElementById('app');

if (!target) {
    throw new Error('Unable to find the application mount point.');
}

async function bootstrap(mountTarget: HTMLElement): Promise<void> {
    await installDiagnostics();
    if ('__TAURI_INTERNALS__' in window) {
        try {
            window.__AXKLIB_SERVER__ =
                (await invoke<Window['__AXKLIB_SERVER__'] | null>('server_connection')) ?? undefined;
            reportInfo(
                window.__AXKLIB_SERVER__
                    ? `Connected to ${window.__AXKLIB_SERVER__.mode} axklib-server.`
                    : 'No axklib-server connection is available.',
            );
        } catch (error) {
            window.__AXKLIB_SERVER__ = undefined;
            reportError('axklib-server is unavailable', error);
        }
    }
    mount(App, { target: mountTarget });
}

void bootstrap(target);
