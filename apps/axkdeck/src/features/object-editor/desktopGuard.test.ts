import { beforeEach, describe, expect, it, vi } from 'vitest';
const mocks = vi.hoisted(() => ({
    invoke: vi.fn().mockResolvedValue(undefined),
    destroy: vi.fn().mockResolvedValue(undefined),
    unclose: vi.fn(),
    unquit: vi.fn(),
    onclose: null as null | ((event: { preventDefault: () => void }) => void),
    onquit: null as null | (() => void),
}));
vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));
vi.mock('@tauri-apps/api/window', () => ({
    getCurrentWindow: () => ({
        destroy: mocks.destroy,
        onCloseRequested: async (callback: typeof mocks.onclose) => {
            mocks.onclose = callback;
            return mocks.unclose;
        },
    }),
}));
vi.mock('@tauri-apps/api/event', () => ({
    listen: async (_: string, callback: typeof mocks.onquit) => {
        mocks.onquit = callback;
        return mocks.unquit;
    },
}));
import { installDesktopEditorGuard } from './desktopGuard';
describe('desktop editor exit protection', () => {
    beforeEach(() => vi.clearAllMocks());
    it('cancels close, then clears native protection before confirmed close', async () => {
        const confirm = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true);
        const guard = await installDesktopEditorGuard(confirm, vi.fn());
        guard.setBlocked(true);
        const event = { preventDefault: vi.fn() };
        mocks.onclose!(event);
        await vi.waitFor(() => expect(confirm).toHaveBeenCalledTimes(1));
        expect(event.preventDefault).toHaveBeenCalled();
        expect(mocks.destroy).not.toHaveBeenCalled();
        mocks.onclose!(event);
        await vi.waitFor(() => expect(mocks.destroy).toHaveBeenCalledOnce());
        expect(mocks.invoke).toHaveBeenLastCalledWith('set_editor_exit_guard', { blocked: false });
        guard.dispose();
        expect(mocks.unclose).toHaveBeenCalledOnce();
        expect(mocks.unquit).toHaveBeenCalledOnce();
    });
    it('guards application Quit separately from window close', async () => {
        const confirm = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true);
        const guard = await installDesktopEditorGuard(confirm, vi.fn());
        mocks.onquit!();
        await vi.waitFor(() => expect(confirm).toHaveBeenCalledTimes(1));
        expect(mocks.invoke).not.toHaveBeenCalled();
        mocks.onquit!();
        await vi.waitFor(() => expect(mocks.invoke).toHaveBeenCalledWith('approve_editor_exit'));
        expect(mocks.destroy).not.toHaveBeenCalled();
        guard.dispose();
    });
    it('reports a native close failure and allows another close attempt', async () => {
        const error = new Error('Native close failed');
        mocks.destroy.mockRejectedValueOnce(error);
        const confirm = vi.fn().mockResolvedValue(true);
        const onerror = vi.fn();
        const guard = await installDesktopEditorGuard(confirm, onerror);
        const event = { preventDefault: vi.fn() };
        mocks.onclose!(event);
        await vi.waitFor(() => expect(onerror).toHaveBeenCalledWith(error));
        mocks.onclose!(event);
        await vi.waitFor(() => expect(mocks.destroy).toHaveBeenCalledTimes(2));
        expect(confirm).toHaveBeenCalledTimes(2);
        expect(event.preventDefault).toHaveBeenCalledTimes(2);
        guard.dispose();
    });
});
