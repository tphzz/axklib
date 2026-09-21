import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import { ASeriesPreferences, type ASeriesGeneration } from '../aSeriesPreferences.svelte';
import PreferencesDialog from './PreferencesDialog.svelte';

async function createPreferences(generation: ASeriesGeneration = 'A3000') {
    const save = vi.fn().mockResolvedValue(undefined);
    const preferences = new ASeriesPreferences({ load: async () => generation, save });
    await preferences.ready;
    return { preferences, save };
}

describe('PreferencesDialog', () => {
    it.each([
        { generation: 'A3000' as const, label: 'a3k' },
        { generation: 'A4000_A5000' as const, label: 'a4k/a5k' },
    ])('starts with the saved $label generation and A3k first', async ({ generation, label }) => {
        const { preferences, save } = await createPreferences(generation);
        render(PreferencesDialog, { props: { preferences, oncancel: vi.fn(), onsaved: vi.fn() } });

        const choices = screen.getByRole('group', { name: 'Preferred A-Series generation' });
        expect(choices.classList).toContain('dialog-segmented-control');
        expect(
            within(choices)
                .getAllByRole('button')
                .map((button) => button.textContent?.trim()),
        ).toEqual(['a3k', 'a4k/a5k']);
        expect(within(choices).getByRole('button', { name: label }).getAttribute('aria-pressed')).toBe('true');
        expect(save).not.toHaveBeenCalled();
    });

    it.each(['Cancel', 'Close'])('discards an unsaved selection through %s', async (action) => {
        const { preferences, save } = await createPreferences();
        const oncancel = vi.fn();
        const onsaved = vi.fn();
        render(PreferencesDialog, { props: { preferences, oncancel, onsaved } });

        await fireEvent.click(screen.getByRole('button', { name: 'a4k/a5k' }));
        expect(preferences.generation).toBe('A3000');
        await fireEvent.click(screen.getByRole('button', { name: action }));

        expect(oncancel).toHaveBeenCalledOnce();
        expect(onsaved).not.toHaveBeenCalled();
        expect(save).not.toHaveBeenCalled();
        expect(preferences.generation).toBe('A3000');
    });

    it('publishes and closes only after the selected generation is saved', async () => {
        const { preferences, save } = await createPreferences();
        let resolveSave: () => void = () => undefined;
        const pending = new Promise<void>((resolve) => {
            resolveSave = resolve;
        });
        save.mockReturnValueOnce(pending);
        const oncancel = vi.fn();
        const onsaved = vi.fn();
        render(PreferencesDialog, { props: { preferences, oncancel, onsaved } });
        await fireEvent.click(screen.getByRole('button', { name: 'a4k/a5k' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Save' }));
        await waitFor(() => expect(save).toHaveBeenCalledExactlyOnceWith('A4000_A5000'));
        expect(preferences.generation).toBe('A3000');
        expect(onsaved).not.toHaveBeenCalled();
        for (const name of ['Save', 'Cancel', 'Close', 'a3k', 'a4k/a5k']) {
            expect((screen.getByRole('button', { name }) as HTMLButtonElement).disabled).toBe(true);
        }
        await fireEvent.keyDown(screen.getByRole('dialog', { name: 'Preferences' }), { key: 'Escape' });
        expect(oncancel).not.toHaveBeenCalled();

        resolveSave();

        await waitFor(() => expect(onsaved).toHaveBeenCalledOnce());
        expect(preferences.generation).toBe('A4000_A5000');
        expect(oncancel).not.toHaveBeenCalled();
    });

    it('keeps failed saves open without publishing and retries the retained draft', async () => {
        const { preferences, save } = await createPreferences();
        save.mockRejectedValueOnce(new Error('Preferences could not be written'));
        const oncancel = vi.fn();
        const onsaved = vi.fn();
        render(PreferencesDialog, { props: { preferences, oncancel, onsaved } });
        await fireEvent.click(screen.getByRole('button', { name: 'a4k/a5k' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Save' }));

        await waitFor(() =>
            expect(screen.getByRole('status').textContent).toContain('Preferences could not be written'),
        );
        expect(screen.getByRole('dialog', { name: 'Preferences' })).toBeTruthy();
        expect(preferences.generation).toBe('A3000');
        expect(onsaved).not.toHaveBeenCalled();
        expect(oncancel).not.toHaveBeenCalled();
        expect(screen.getByRole('button', { name: 'a4k/a5k' }).getAttribute('aria-pressed')).toBe('true');
        expect((screen.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(false);

        await fireEvent.click(screen.getByRole('button', { name: 'Save' }));

        await waitFor(() => expect(onsaved).toHaveBeenCalledOnce());
        expect(save.mock.calls).toEqual([['A4000_A5000'], ['A4000_A5000']]);
        expect(preferences.generation).toBe('A4000_A5000');
    });

    it('disables editing and saving when persisted preferences cannot be loaded', async () => {
        const save = vi.fn();
        const preferences = new ASeriesPreferences({
            load: async () => {
                throw new Error('Preferences could not be read');
            },
            save,
        });
        await preferences.ready;
        const oncancel = vi.fn();
        const onsaved = vi.fn();
        render(PreferencesDialog, { props: { preferences, oncancel, onsaved } });

        expect(screen.getByRole('status').textContent).toContain('Preferences could not be read');
        for (const name of ['Save', 'a3k', 'a4k/a5k']) {
            expect((screen.getByRole('button', { name }) as HTMLButtonElement).disabled).toBe(true);
        }
        await fireEvent.click(screen.getByRole('button', { name: 'Save' }));
        expect(save).not.toHaveBeenCalled();
        expect(onsaved).not.toHaveBeenCalled();
        expect(preferences.generation).toBe('A3000');
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
        expect(oncancel).toHaveBeenCalledOnce();
    });
});
