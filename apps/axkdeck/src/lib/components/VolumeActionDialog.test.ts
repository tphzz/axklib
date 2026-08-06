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

const safeDeletion = {
    imageId: 'image-1',
    revision: 1,
    partitionIndex: 2,
    volumeName: 'ORIGINAL',
    canDelete: true,
    crossingRelationshipCount: 0,
    blockers: [],
};

describe('VolumeActionDialog', () => {
    it('shows the active volume operation while submission is pending', () => {
        render(VolumeActionDialog, {
            props: {
                action: 'add-volume',
                item: { ...volume, kind: 'partition' },
                busy: true,
                phase: 'submitting',
                error: '',
                deletionInspection: null,
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
                phase: 'idle',
                error: '',
                deletionInspection: null,
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
            props: {
                action: 'delete-volume',
                item: volume,
                busy: false,
                phase: 'idle',
                error: '',
                deletionInspection: safeDeletion,
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const submit = screen.getByRole('button', { name: 'Delete permanently' });
        expect((submit as HTMLButtonElement).disabled).toBe(false);
        expect(screen.queryByLabelText('Volume name confirmation')).toBeNull();
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('ORIGINAL');
    });

    it('points to the explicit placement repair action when deletion is blocked', () => {
        render(VolumeActionDialog, {
            props: {
                action: 'delete-volume',
                item: volume,
                busy: false,
                phase: 'idle',
                error: '',
                deletionInspection: {
                    ...safeDeletion,
                    canDelete: false,
                    crossingRelationshipCount: 2,
                    blockers: [{ code: 'KNOWN_RELATIONSHIP_CROSSES_VOLUME', message: 'Crossing link', count: 2 }],
                },
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        expect(screen.getByText(/Repair object placement from the volume or partition context menu/)).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Delete permanently' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.queryByRole('button', { name: 'Repair placement' })).toBeNull();
    });

    it('validates and submits partition renames with the same naming rules as volumes', async () => {
        const onsubmit = vi.fn();
        render(VolumeActionDialog, {
            props: {
                action: 'rename-partition',
                item: { ...volume, id: 'partition-2', name: 'PARTITION 3', kind: 'partition' },
                busy: false,
                phase: 'idle',
                error: '',
                deletionInspection: null,
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const input = screen.getByLabelText('Partition name');
        await Promise.resolve();
        expect(document.activeElement).toBe(input);
        expect((input as HTMLInputElement).selectionStart).toBe(0);
        expect((input as HTMLInputElement).selectionEnd).toBe('PARTITION 3'.length);
        const submit = screen.getByRole('button', { name: 'Rename' });
        expect((input as HTMLInputElement).value).toBe('PARTITION 3');
        expect((submit as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(input, { target: { value: 'Samples' } });
        expect((submit as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('Samples');
    });
});
