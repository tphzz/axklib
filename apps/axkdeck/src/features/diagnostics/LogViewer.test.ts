import { cleanup, fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import LogViewer from './LogViewer.svelte';
import OpenLogsButton from './OpenLogsButton.svelte';
import { invoke } from '@tauri-apps/api/core';
import type { LogDriver, LogPage } from './contracts';

vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
});

function fixture() {
    const page: LogPage = {
        entries: [
            { id: 1, source: 'application', level: 'error', timestamp: 1790575200000, text: 'Failure\n  stack frame' },
        ],
        total: 1,
        newCount: 0,
        sequence: 1,
        historyChanges: 0,
        olderCursor: null,
        newerCursor: null,
    };
    const driver: LogDriver = {
        read: vi.fn().mockResolvedValue(page),
        clear: vi.fn().mockResolvedValue(4),
        save: vi.fn().mockResolvedValue(null),
    };
    return { driver, page, view: render(LogViewer, { driver }) };
}

describe('Logs window', () => {
    it('combines controls in the backend query and uses a non-destructive clear boundary', async () => {
        const { driver, view } = fixture();
        await view.findByText('Failure', { exact: false });
        await fireEvent.change(view.getByLabelText('Minimum level'), { target: { value: 'warning' } });
        await fireEvent.change(view.getByLabelText('Source'), { target: { value: 'localServer' } });
        await fireEvent.input(view.getByLabelText('Search'), { target: { value: 'failure' } });
        await waitFor(() =>
            expect(vi.mocked(driver.read).mock.lastCall?.[0].filter).toMatchObject({
                minimumLevel: 'warning',
                source: 'localServer',
                search: 'failure',
            }),
        );
        await fireEvent.click(view.getByRole('button', { name: 'Clear view' }));
        await waitFor(() => expect(vi.mocked(driver.read).mock.lastCall?.[0].filter.since).toBe(4));
        await fireEvent.click(view.getByRole('button', { name: 'Show retained history' }));
        await waitFor(() => expect(vi.mocked(driver.read).mock.lastCall?.[0].filter.since).toBeNull());
    });

    it('copies the exact selected text and suspends follow without live-region announcements', async () => {
        const { view } = fixture();
        await view.findByText('Failure', { exact: false });
        const writeText = vi.fn().mockResolvedValue(undefined);
        Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText } });
        const text = view.container.querySelector('.log-text')!;
        const range = document.createRange();
        range.selectNodeContents(text);
        window.getSelection()!.removeAllRanges();
        window.getSelection()!.addRange(range);
        await fireEvent(document, new Event('selectionchange'));
        expect(view.getByRole('switch', { name: 'Follow latest' }).getAttribute('aria-checked')).toBe('false');
        await fireEvent.click(view.getByRole('button', { name: 'Copy selection' }));
        expect(writeText).toHaveBeenCalledWith('Failure\n  stack frame');
        expect(view.getByRole('textbox', { name: 'Log entries' }).getAttribute('aria-live')).toBeNull();
        writeText.mockRejectedValueOnce(new Error('clipboard denied'));
        await fireEvent.click(view.getByRole('button', { name: 'Copy selection' }));
        await view.findByText('Error: clipboard denied');
        expect(window.getSelection()!.toString()).toBe('Failure\n  stack frame');
        range.selectNodeContents(view.container.querySelector('.log-footer')!);
        window.getSelection()!.removeAllRanges();
        window.getSelection()!.addRange(range);
        await fireEvent(document, new Event('selectionchange'));
        expect(view.getByRole('button', { name: 'Copy selection' }).hasAttribute('disabled')).toBe(true);
    });

    it('preserves records whose timestamp exceeds the supported date range', async () => {
        const { page, view } = fixture();
        page.entries[0].timestamp = 9223372036854775807;
        await view.findByText('Unknown time');
        expect(view.getByText('Failure', { exact: false })).toBeTruthy();
    });

    it('supports keyboard save-menu navigation, cancellation, and separate export scopes', async () => {
        const { driver, view } = fixture();
        await view.findByText('Failure', { exact: false });
        const save = view.getByRole('button', { name: 'Save logs' });
        await fireEvent.click(save);
        const current = view.getByRole('menuitem', { name: 'Save view...' });
        expect(document.activeElement).toBe(current);
        await fireEvent.keyDown(current, { key: 'ArrowDown' });
        expect(document.activeElement).toBe(view.getByRole('menuitem', { name: 'Save all logs...' }));
        await fireEvent.keyDown(document.activeElement!, { key: 'Escape' });
        expect(document.activeElement).toBe(save);
        expect(view.queryByRole('menu')).toBeNull();
        await fireEvent.click(save);
        await fireEvent.click(view.getByRole('menuitem', { name: 'Save view...' }));
        await waitFor(() => expect(driver.save).toHaveBeenCalledWith(expect.objectContaining({ since: null })));
        await waitFor(() => expect(save.hasAttribute('disabled')).toBe(false));
        await fireEvent.click(save);
        await fireEvent.click(view.getByRole('menuitem', { name: 'Save all logs...' }));
        await waitFor(() => expect(driver.save).toHaveBeenLastCalledWith(null));
        expect(view.container.querySelector('.log-footer .failure')).toBeNull();
    });

    it('opens the desktop singleton and surfaces an open failure', async () => {
        vi.mocked(invoke).mockRejectedValueOnce('window failed');
        const view = render(OpenLogsButton);
        await fireEvent.click(view.getByRole('button', { name: 'Logs...' }));
        await view.findByRole('alert');
        expect(invoke).toHaveBeenCalledWith('open_diagnostic_logs');
        expect(view.getByRole('alert').textContent).toContain('window failed');
    });
});
