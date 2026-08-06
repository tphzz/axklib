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
