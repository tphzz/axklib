import { invoke } from '@tauri-apps/api/core';
import { mount, tick } from 'svelte';
import MappingWindow from './MappingWindow.svelte';
import { mappingWindowAdapter } from './desktop';
import { installDiagnostics, reportDiagnostic } from '../../lib/diagnostics';
import { createInterfaceScaleController, type InterfaceScaleMode } from '../../lib/interfaceScale';
import { createTauriInterfaceScaleAdapter, showCurrentTauriWindow } from '../../lib/tauriInterfaceScale';
import { revealAfterInterfaceScale, waitForVisibleScaleCommit } from '../../lib/startupVisibility';

export async function startMappingView(target: HTMLElement, desktop: boolean): Promise<void> {
    try {
        if (!desktop) {
            target.textContent = 'Mapping Editor is available in the desktop application.';
            return;
        }
        await installDiagnostics();
        const mode = await invoke<InterfaceScaleMode>('desktop_interface_scale_mode');
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
        mount(MappingWindow, { target, props: { adapter: mappingWindowAdapter } });
    } catch (error) {
        reportDiagnostic('mapping_startup_failed', { message: String(error) }, 'error');
        target.textContent = `Mapping Editor could not be loaded: ${String(error)}`;
    } finally {
        await revealAfterInterfaceScale(Promise.resolve(), tick, {
            showWindow: desktop ? showCurrentTauriWindow : undefined,
            waitForScaleCommit: desktop ? waitForVisibleScaleCommit : undefined,
        });
    }
}
