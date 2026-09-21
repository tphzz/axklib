import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { expect, it } from 'vitest';
import Harness from '../../test/WorkspacePreferencesHarness.svelte';

it('opens Preferences outside the filtered header and restores focus after cancel and save', async () => {
    render(Harness);
    const trigger = screen.getByRole('button', { name: 'Preferences' }) as HTMLButtonElement;
    await waitFor(() => expect(trigger.disabled).toBe(false));
    trigger.focus();
    await fireEvent.click(trigger);
    let dialog = await screen.findByRole('dialog', { name: 'Preferences' });
    expect(dialog.closest('.app-header')).toBeNull();
    await fireEvent.click(within(dialog).getByRole('button', { name: 'a4k/a5k' }));
    await fireEvent.keyDown(dialog, { key: 'Escape' });
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect(document.activeElement).toBe(trigger);
    expect(screen.getByLabelText('Saved generation').textContent).toBe('A3000');

    await fireEvent.click(trigger);
    dialog = await screen.findByRole('dialog', { name: 'Preferences' });
    await fireEvent.click(within(dialog).getByRole('button', { name: 'a4k/a5k' }));
    await fireEvent.click(within(dialog).getByRole('button', { name: 'Save' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect(document.activeElement).toBe(trigger);
    expect(screen.getByLabelText('Saved generation').textContent).toBe('A4000_A5000');
});
