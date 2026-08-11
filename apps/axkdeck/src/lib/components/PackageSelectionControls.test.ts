import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import PackageSelectionControls from './PackageSelectionControls.svelte';

describe('PackageSelectionControls', () => {
    it('exposes the persistent selection count with export, deletion, and clear actions', async () => {
        const onexportpackage = vi.fn();
        const onexportsfz = vi.fn();
        const onexportmidi = vi.fn();
        const ondelete = vi.fn();
        const onclear = vi.fn();
        render(PackageSelectionControls, {
            props: { count: 4, onexportpackage, onexportsfz, onexportmidi, ondelete, onclear },
        });

        expect(screen.getByRole('status').textContent).toBe('4 selected');
        await fireEvent.click(screen.getByRole('button', { name: 'Export 4 selected objects' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Export 4 selected objects' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export SFZ…' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Export 4 selected objects' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export MIDI…' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Delete 4 selected objects' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Clear object selection' }));
        expect(onexportpackage).toHaveBeenCalledOnce();
        expect(onexportsfz).toHaveBeenCalledOnce();
        expect(onexportmidi).toHaveBeenCalledOnce();
        expect(ondelete).toHaveBeenCalledOnce();
        expect(onclear).toHaveBeenCalledOnce();
    });
});
