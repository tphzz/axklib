import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import PlacementRepairDialog from './PlacementRepairDialog.svelte';

const partition = {
    id: 'partition-0',
    name: 'Single-Cycle',
    kind: 'partition' as const,
    childCount: 21,
    partitionIndex: 0,
};

const inspection = {
    imageId: 'image-1',
    revision: 3,
    scope: { kind: 'PARTITION' as const, partitionIndex: 0 },
    canRepair: true,
    repairObjectCount: 62,
    blockedObjectCount: 0,
    recoveryVolumeName: 'Recovered',
    destinations: [
        {
            volumeName: 'Recovered',
            createsVolume: true,
            objectCount: 62,
            objectTypeCounts: { SMPL: 62 },
        },
    ],
    blockers: [],
};

describe('PlacementRepairDialog', () => {
    const props = {
        item: partition,
        inspection,
        busy: false,
        phase: 'idle' as const,
        error: '',
        message: '',
        oncancel: vi.fn(),
        onsubmit: vi.fn(),
    };

    it.each(['inspecting', 'repairing'] as const)('locks every dismissal route while %s', async (phase) => {
        const oncancel = vi.fn();
        const onsubmit = vi.fn();
        render(PlacementRepairDialog, { ...props, busy: true, phase, oncancel, onsubmit });
        const dialog = screen.getByRole('dialog');
        await fireEvent.keyDown(dialog, { key: 'Escape' });
        (screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).click();
        (screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).click();
        expect(oncancel).not.toHaveBeenCalled();
        const submit = screen.getByRole('button', { name: 'Repair placement' }) as HTMLButtonElement;
        submit.click();
        expect(submit.disabled).toBe(true);
        expect(onsubmit).not.toHaveBeenCalled();
        expect(dialog.querySelector('.dialog-footer-status')?.textContent).toContain(
            phase === 'inspecting' ? 'Inspecting' : 'Repairing',
        );
    });

    it('keeps validation editable and uses the shared field/footer', async () => {
        const onsubmit = vi.fn();
        render(PlacementRepairDialog, { ...props, onsubmit });
        const field = screen.getByLabelText('Recovery volume name');
        expect(field.classList.contains('dialog-field-control')).toBe(true);
        await fireEvent.input(field, { target: { value: ' Bad name ' } });
        const submit = screen.getByRole('button', { name: 'Repair placement' }) as HTMLButtonElement;
        expect(submit.disabled).toBe(true);
        submit.click();
        expect(onsubmit).not.toHaveBeenCalled();
        expect(screen.getByRole('dialog').querySelector('.dialog-footer-actions')).toBeTruthy();
    });

    it('retains completion with Done, and informational results with Close', async () => {
        const view = render(PlacementRepairDialog, { ...props, message: 'Repaired 62 objects.' });
        expect(screen.getByRole('button', { name: 'Done' })).toBeTruthy();
        expect(screen.queryByRole('button', { name: 'Repair placement' })).toBeNull();
        expect(screen.getByRole('dialog').querySelector('.dialog-footer-status')?.textContent).toContain(
            'Repair completed',
        );
        expect(screen.getByText('Repaired 62 objects.').closest('.placement-results')).toBeTruthy();
        expect(screen.getByRole('region', { name: 'Repair details' }).tabIndex).toBe(0);
        await view.rerender({ ...props, inspection: { ...inspection, canRepair: false } });
        expect(screen.getAllByRole('button', { name: 'Close' })).toHaveLength(2);
    });

    it('retains failures and allows idle dismissal without inventing recovery actions', async () => {
        const oncancel = vi.fn();
        render(PlacementRepairDialog, { ...props, error: 'Inspection failed', oncancel });
        expect(screen.getByRole('alert').textContent).toContain('failed');
        expect(screen.getByText('Inspection failed').closest('.placement-results')).toBeTruthy();
        await fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('explains and submits a partition recovery volume', async () => {
        const onsubmit = vi.fn();
        render(PlacementRepairDialog, {
            props: {
                item: partition,
                inspection,
                busy: false,
                phase: 'idle',
                error: '',
                message: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        expect(screen.getByText('62 objects can be repaired')).toBeTruthy();
        expect(screen.getByText('62 Wave Data')).toBeTruthy();
        expect((screen.getByLabelText('Recovery volume name') as HTMLInputElement).value).toBe('Recovered');
        await fireEvent.click(screen.getByRole('button', { name: 'Repair placement' }));
        expect(onsubmit).toHaveBeenCalledWith('Recovered');
    });

    it('keeps ambiguous objects unchanged while allowing safe repairs', () => {
        render(PlacementRepairDialog, {
            props: {
                item: partition,
                inspection: {
                    ...inspection,
                    blockedObjectCount: 3,
                    blockers: [
                        {
                            code: 'PLACEMENT_MULTIPLE_VOLUME_OWNERS',
                            message: 'Three objects have more than one possible owner.',
                            count: 3,
                        },
                    ],
                },
                busy: false,
                phase: 'idle',
                error: '',
                message: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        expect(screen.getByText('3 objects will remain unchanged')).toBeTruthy();
        expect(screen.getByText('Three objects have more than one possible owner.')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Repair placement' }) as HTMLButtonElement).disabled).toBe(false);
    });
});
