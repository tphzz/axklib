import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import PackageSelectionControls from './PackageSelectionControls.svelte';

describe('PackageSelectionControls', () => {
    it('exposes the persistent selection count with export and clear actions', async () => {
        const onexport = vi.fn();
        const onclear = vi.fn();
        render(PackageSelectionControls, { props: { count: 4, onexport, onclear } });

        expect(screen.getByRole('status').textContent).toBe('4 selected');
        await fireEvent.click(screen.getByRole('button', { name: 'Export 4 selected objects' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Clear object selection' }));
        expect(onexport).toHaveBeenCalledOnce();
        expect(onclear).toHaveBeenCalledOnce();
    });
});
