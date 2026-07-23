import './app.css';
import { mount } from 'svelte';
import App from './App.svelte';
import { invoke } from '@tauri-apps/api/core';
import { installDiagnostics, reportDiagnostic, reportError, reportInfo } from './lib/diagnostics';
import { createInterfaceScaleController, type InterfaceScaleController } from './lib/interfaceScale';
import { createTauriInterfaceScaleAdapter } from './lib/tauriInterfaceScale';

const target = document.getElementById('app');

if (!target) {
    throw new Error('Unable to find the application mount point.');
}

async function bootstrap(mountTarget: HTMLElement): Promise<void> {
    await installDiagnostics();
    const isDesktop = '__TAURI_INTERNALS__' in window;
    let interfaceScaling: InterfaceScaleController | null = null;
    if (isDesktop) {
        try {
            interfaceScaling = await createInterfaceScaleController(
                createTauriInterfaceScaleAdapter(),
                window.localStorage,
                reportDiagnostic,
            );
        } catch (error) {
            reportDiagnostic('interface_scale_initialization_failed', { message: String(error) }, 'warn');
        }
    }
    if (isDesktop) {
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
    mount(App, { target: mountTarget, props: { interfaceScaling } });
}

void bootstrap(target);
