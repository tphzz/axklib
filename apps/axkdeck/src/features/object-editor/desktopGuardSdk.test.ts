import { afterEach, describe, expect, it, vi } from 'vitest';
import { clearMocks, mockIPC, mockWindows } from '@tauri-apps/api/mocks';
import { emit, TauriEvent } from '@tauri-apps/api/event';
import capability from '../../../src-tauri/capabilities/editor-close.json';
import { installDesktopEditorGuard } from './desktopGuard';

// Keep the real Window.onCloseRequested implementation: it performs a destroy
// command after an unprevented close, which a mock of Window.close would hide.
function nativeBoundary() {
    const commands: string[] = [];
    const denied: string[] = [];
    mockWindows('main');
    mockIPC(
        async (command) => {
            commands.push(command);
            if (command === 'plugin:window|close' || command === 'plugin:window|destroy') {
                const permission = `core:window:allow-${command.split('|')[1]}`;
                if (!capability.permissions.includes(permission)) denied.push(permission);
                if (command === 'plugin:window|close') await emit(TauriEvent.WINDOW_CLOSE_REQUESTED);
            }
        },
        { shouldMockEvents: true },
    );
    return { commands, denied };
}

describe('desktop editor close through the Tauri SDK', () => {
    afterEach(() => clearMocks());

    it('can actually destroy the main window after an approved close', async () => {
        const { commands, denied } = nativeBoundary();
        const onerror = vi.fn();
        const confirm = vi.fn().mockResolvedValue(true);
        const guard = await installDesktopEditorGuard(confirm, onerror);
        await emit(TauriEvent.WINDOW_CLOSE_REQUESTED);
        await vi.waitFor(() => expect(commands).toContain('plugin:window|destroy'));
        expect(denied).toEqual([]);
        expect(capability.windows).toEqual(['main']);
        expect(confirm).toHaveBeenCalledOnce();
        expect(onerror).not.toHaveBeenCalled();
        expect(commands.indexOf('set_editor_exit_guard')).toBeLessThan(commands.indexOf('plugin:window|destroy'));
        guard.dispose();
    });

    it('does not destroy the window while confirmation is pending or cancelled', async () => {
        const { commands } = nativeBoundary();
        let resolve!: (approved: boolean) => void;
        const confirm = vi.fn(
            () =>
                new Promise<boolean>((done) => {
                    resolve = done;
                }),
        );
        const guard = await installDesktopEditorGuard(confirm, vi.fn());
        await emit(TauriEvent.WINDOW_CLOSE_REQUESTED);
        expect(confirm).toHaveBeenCalledOnce();
        expect(commands).toEqual([]);
        resolve(false);
        await new Promise((done) => setTimeout(done, 0));
        expect(commands).toEqual([]);
        guard.dispose();
    });
});
