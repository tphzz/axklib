import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import VolumeActionDialog from './VolumeActionDialog.svelte';

const volume = {
    id: 'volume-1',
    name: 'ORIGINAL',
    kind: 'volume' as const,
    childCount: 0,
    partitionIndex: 2,
};

describe('VolumeActionDialog', () => {
    it('shows the active volume operation while submission is pending', () => {
        render(VolumeActionDialog, {
            props: {
                action: 'add-volume',
                item: { ...volume, kind: 'partition' },
                busy: true,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const submit = screen.getByRole('button', { name: 'Adding' });
        expect((submit as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('enforces Yamaha volume-name limits before adding a volume', async () => {
        const onsubmit = vi.fn();
        render(VolumeActionDialog, {
            props: {
                action: 'add-volume',
                item: { ...volume, kind: 'partition' },
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const submit = screen.getByRole('button', { name: 'Add' });
        expect((submit as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(screen.getByLabelText('Volume name'), { target: { value: 'VOLUME 2' } });
        expect((submit as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('VOLUME 2');
    });

    it('uses one explicit warning without typed-name confirmation for permanent deletion', async () => {
        const onsubmit = vi.fn();
        render(VolumeActionDialog, {
            props: { action: 'delete-volume', item: volume, busy: false, error: '', oncancel: vi.fn(), onsubmit },
        });

        const submit = screen.getByRole('button', { name: 'Delete permanently' });
        expect((submit as HTMLButtonElement).disabled).toBe(false);
        expect(screen.queryByLabelText('Volume name confirmation')).toBeNull();
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('ORIGINAL');
    });

    it('validates and submits partition renames with the same naming rules as volumes', async () => {
        const onsubmit = vi.fn();
        render(VolumeActionDialog, {
            props: {
                action: 'rename-partition',
                item: { ...volume, id: 'partition-2', name: 'PARTITION 3', kind: 'partition' },
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const input = screen.getByLabelText('Partition name');
        const submit = screen.getByRole('button', { name: 'Rename' });
        expect((input as HTMLInputElement).value).toBe('PARTITION 3');
        expect((submit as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(input, { target: { value: 'Samples' } });
        expect((submit as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('Samples');
    });
});
