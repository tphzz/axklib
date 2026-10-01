import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';

export async function installDesktopEditorGuard(confirm: () => Promise<boolean>, onerror: (error: unknown) => void) {
    const window = getCurrentWindow();
    let closing = false;
    const request = async (quit: boolean) => {
        if (closing) return;
        try {
            if (!(await confirm())) return;
            closing = true;
            if (quit) await invoke('approve_editor_exit');
            else {
                await invoke('set_editor_exit_guard', { blocked: false });
                await window.destroy();
            }
        } catch (error) {
            closing = false;
            onerror(error);
        }
    };
    const unclose = await window.onCloseRequested((event) => {
        event.preventDefault();
        void request(false);
    });
    let unquit: () => void;
    try {
        unquit = await listen('editor-exit-requested', () => void request(true));
    } catch (error) {
        unclose();
        throw error;
    }
    return {
        setBlocked: (blocked: boolean) => {
            void invoke('set_editor_exit_guard', { blocked }).catch(onerror);
        },
        dispose: () => {
            unclose();
            unquit();
        },
    };
}
