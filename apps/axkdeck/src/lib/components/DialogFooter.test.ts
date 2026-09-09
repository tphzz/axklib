import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import StoragePickerFooter from './StoragePickerFooter.svelte';
import ImportUnavailableDialog from './ImportUnavailableDialog.svelte';

const styles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
const callbacks = {
    onloadmore: vi.fn(),
    onoutput: vi.fn(),
    ondirectory: vi.fn(),
    onmedia: vi.fn(),
    onfiles: vi.fn(),
    oncancel: vi.fn(),
};

describe('Dialog action contract', () => {
    it.each(['directory', 'save-file', 'save-directory', 'media-source', 'file'] as const)(
        'puts dismissal before confirmation in the %s picker',
        (mode) => {
            const view = render(StoragePickerFooter, {
                props: {
                    ...callbacks,
                    mode,
                    hasDirectory: true,
                    hasMore: false,
                    multiple: true,
                    loading: false,
                    opening: false,
                    selectedCount: 2,
                    outputName: 'Export',
                },
            });
            const actions = view.container.querySelector('.dialog-footer-actions')!;
            expect(actions.querySelectorAll('button')[0].textContent?.trim()).toBe('Cancel');
            expect(actions.querySelectorAll('button')[1].classList.contains('primary-button')).toBe(true);
            const style = document.createElement('style');
            style.textContent =
                styles.match(
                    /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
                )?.[0] ?? '';
            document.head.append(style);
            try {
                for (const button of actions.querySelectorAll('button')) {
                    expect(getComputedStyle(button).height).toBe('30px');
                    expect(getComputedStyle(button).marginTop).toBe('0px');
                    expect(getComputedStyle(button).marginBottom).toBe('0px');
                }
            } finally {
                style.remove();
            }
        },
    );
    it('uses Close for information rather than a generic acknowledgement', async () => {
        const onclose = vi.fn();
        render(ImportUnavailableDialog, {
            props: { title: 'Import unavailable', message: 'Read only image', onclose },
        });
        expect(screen.queryByRole('button', { name: 'OK' })).toBeNull();
        await fireEvent.click(screen.getAllByRole('button', { name: 'Close' }).at(-1)!);
        expect(onclose).toHaveBeenCalledOnce();
    });
});
