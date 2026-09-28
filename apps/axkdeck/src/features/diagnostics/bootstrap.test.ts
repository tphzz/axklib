import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { mount } from 'svelte';
import { invoke } from '@tauri-apps/api/core';
import { startLogsView } from './bootstrap';
import LogViewer from './LogViewer.svelte';
import { showCurrentTauriWindow } from '../../lib/tauriInterfaceScale';
import { reportDiagnostic } from '../../lib/diagnostics';
import { createInterfaceScaleController } from '../../lib/interfaceScale';

const native = vi.hoisted(() => ({ dispose: vi.fn().mockResolvedValue(undefined) }));
vi.mock('svelte', async (original) => ({ ...(await original<typeof import('svelte')>()), mount: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
vi.mock('@tauri-apps/api/event', () => ({ listen: vi.fn().mockResolvedValue(() => undefined) }));
vi.mock('../../lib/diagnostics', () => ({
    installDiagnostics: vi.fn().mockResolvedValue(undefined),
    reportDiagnostic: vi.fn(),
}));
vi.mock('../../lib/interfaceScale', () => ({
    createInterfaceScaleController: vi.fn().mockResolvedValue({ dispose: native.dispose }),
}));
vi.mock('../../lib/tauriInterfaceScale', () => ({
    createTauriInterfaceScaleAdapter: vi.fn(),
    showCurrentTauriWindow: vi.fn().mockResolvedValue(undefined),
}));
beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(invoke).mockResolvedValue('auto');
    document.documentElement.setAttribute('data-interface-scale-pending', '');
    vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => {
        queueMicrotask(() => callback(0));
        return 1;
    });
});
afterEach(() => {
    window.dispatchEvent(new Event('pagehide'));
    document.documentElement.removeAttribute('data-interface-scale-pending');
    vi.restoreAllMocks();
});

describe('Logs bootstrap', () => {
    it('mounts independently of server and workspace startup and releases scale listeners', async () => {
        const target = document.createElement('div');
        await startLogsView(target, true);
        expect(mount).toHaveBeenCalledWith(LogViewer, expect.objectContaining({ target }));
        expect(invoke).toHaveBeenCalledTimes(1);
        expect(invoke).toHaveBeenCalledWith('desktop_interface_scale_mode');
        expect(showCurrentTauriWindow).toHaveBeenCalledOnce();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
        window.dispatchEvent(new Event('pagehide'));
        expect(native.dispose).toHaveBeenCalledOnce();
    });

    it('keeps logs available when reading display preferences fails', async () => {
        vi.mocked(invoke).mockRejectedValueOnce(new Error('preferences unreadable'));
        await startLogsView(document.createElement('div'), true);
        expect(mount).toHaveBeenCalledOnce();
        expect(reportDiagnostic).toHaveBeenCalledWith('logs_scale_preference_failed', expect.any(Object), 'warn');
        expect(showCurrentTauriWindow).toHaveBeenCalledOnce();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
    });

    it('shows the native window before waiting for its visible scale frames', async () => {
        const frames: FrameRequestCallback[] = [];
        vi.mocked(window.requestAnimationFrame).mockImplementation((callback) => frames.push(callback));
        const ready = startLogsView(document.createElement('div'), true);
        await vi.waitFor(() => expect(window.requestAnimationFrame).toHaveBeenCalledOnce());
        expect(showCurrentTauriWindow).toHaveBeenCalledOnce();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(true);
        frames.shift()?.(0);
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(true);
        frames.shift()?.(16);
        await ready;
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
    });

    it('reveals the startup error if interface scaling fails', async () => {
        vi.mocked(createInterfaceScaleController).mockRejectedValueOnce(new Error('scale unavailable'));
        const target = document.createElement('div');
        await startLogsView(target, true);
        expect(target.textContent).toContain('The Logs window could not be loaded: Error: scale unavailable');
        expect(mount).not.toHaveBeenCalled();
        expect(showCurrentTauriWindow).toHaveBeenCalledOnce();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
    });

    it('does not expose desktop log access in browser-only builds', async () => {
        const target = document.createElement('div');
        await startLogsView(target, false);
        expect(target.textContent).toContain('desktop application');
        expect(mount).not.toHaveBeenCalled();
        expect(invoke).not.toHaveBeenCalled();
        expect(showCurrentTauriWindow).not.toHaveBeenCalled();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
    });

    it('reveals and logs a viewer mounting failure', async () => {
        vi.mocked(mount).mockImplementationOnce(() => {
            throw new Error('Viewer unavailable');
        });
        const target = document.createElement('div');
        await startLogsView(target, true);
        expect(target.textContent).toContain('Viewer unavailable');
        expect(reportDiagnostic).toHaveBeenCalledWith(
            'logs_startup_failed',
            { message: 'Error: Viewer unavailable' },
            'error',
        );
        expect(showCurrentTauriWindow).toHaveBeenCalledOnce();
        expect(document.documentElement.hasAttribute('data-interface-scale-pending')).toBe(false);
    });
});
