import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { mount, tick } from 'svelte';
import LogViewer from './LogViewer.svelte';
import { installDiagnostics, reportDiagnostic } from '../../lib/diagnostics';
import { createInterfaceScaleController, type InterfaceScaleMode } from '../../lib/interfaceScale';
import { createTauriInterfaceScaleAdapter, showCurrentTauriWindow } from '../../lib/tauriInterfaceScale';
import { revealAfterInterfaceScale, waitForVisibleScaleCommit } from '../../lib/startupVisibility';

export async function startLogsView(target: HTMLElement, isDesktop: boolean): Promise<void> {
    if (!isDesktop) {
        target.textContent = 'Logs are available in the axkdeck desktop application.';
        await revealAfterInterfaceScale(Promise.resolve(), tick);
        return;
    }
    try {
        await installDiagnostics();
        let mode: InterfaceScaleMode = 'auto';
        try {
            mode = await invoke<InterfaceScaleMode>('desktop_interface_scale_mode');
        } catch (error) {
            reportDiagnostic('logs_scale_preference_failed', { message: String(error) }, 'warn');
        }
        const scaling = await createInterfaceScaleController(
            createTauriInterfaceScaleAdapter(),
            mode,
            undefined,
            reportDiagnostic,
        );
        window.addEventListener(
            'pagehide',
            () => {
                void scaling.dispose();
            },
            { once: true },
        );
        mount(LogViewer, {
            target,
            props: {
                subscribeVisibility: (callback) =>
                    listen<boolean>('logs-visibility', (event) => callback(event.payload)),
            },
        });
    } catch (error) {
        reportDiagnostic('logs_startup_failed', { message: String(error) }, 'error');
        target.textContent = `The Logs window could not be loaded: ${String(error)}`;
    }
    await revealAfterInterfaceScale(Promise.resolve(), tick, {
        showWindow: showCurrentTauriWindow,
        waitForScaleCommit: waitForVisibleScaleCommit,
    });
}
