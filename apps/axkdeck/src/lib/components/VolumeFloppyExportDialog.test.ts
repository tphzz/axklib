import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import VolumeFloppyExportDialog from './VolumeFloppyExportDialog.svelte';

const inspection = {
    imageId: 'image-1',
    revision: 1,
    sourceMediaKind: 'SFS' as const,
    scopeId: 'partition-0',
    scopeName: 'Partition 0',
    defaultDirectoryName: 'Partition 0 floppies',
    volumeCount: 3,
    exportableCount: 1,
    emptyCount: 1,
    blockedCount: 1,
    totalFloppyImageCount: 2,
    projectedDiskBytes: 2_949_120,
    volumes: [
        {
            contentId: 'volume-1',
            name: 'Drums',
            displayName: 'Drums',
            partitionIndex: 0,
            volumeDirectoryId: 17,
            objectCount: 4,
            payloadBytes: 1_600_000,
            state: 'READY' as const,
            directoryName: 'Drums',
            floppyImageCount: 2,
            projectedDiskBytes: 2_949_120,
            issues: [],
        },
        {
            contentId: 'volume-2',
            name: 'Empty',
            displayName: 'Empty',
            partitionIndex: 0,
            volumeDirectoryId: 18,
            objectCount: 0,
            payloadBytes: 0,
            state: 'EMPTY' as const,
            directoryName: null,
            floppyImageCount: 0,
            projectedDiskBytes: 0,
            issues: [],
        },
        {
            contentId: 'volume-3',
            name: 'Oversized',
            displayName: 'Oversized',
            partitionIndex: 0,
            volumeDirectoryId: 19,
            objectCount: 1,
            payloadBytes: 40_000_000,
            state: 'BLOCKED' as const,
            directoryName: null,
            floppyImageCount: 0,
            projectedDiskBytes: 0,
            issues: [
                {
                    blocking: true,
                    code: 'floppy_image_limit',
                    message: 'The volume requires more than 32 floppy images',
                    measurement: { required: 33, available: 32, unit: 'FLOPPY_IMAGES' as const },
                },
            ],
        },
    ],
};

describe('VolumeFloppyExportDialog', () => {
    it('shows ready, empty, and blocked outcomes before choosing a destination', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(VolumeFloppyExportDialog, {
            props: {
                scopeName: 'Partition 0',
                inspection,
                desktop: true,
                loading: false,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace,
                onlocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('1 volume')).toBeTruthy();
        expect(screen.getByText('2 floppy images')).toBeTruthy();
        expect(screen.getByText('1 empty')).toBeTruthy();
        expect(screen.getByText('1 blocked')).toBeTruthy();
        expect(screen.getByText('2 disks · Drums')).toBeTruthy();
        expect(screen.getByText('The volume requires more than 32 floppy images')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('does not offer a destination when no volume can produce a floppy set', () => {
        render(VolumeFloppyExportDialog, {
            props: {
                scopeName: 'Partition 0',
                inspection: { ...inspection, exportableCount: 0, totalFloppyImageCount: 0 },
                desktop: true,
                loading: false,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('alert').textContent).toContain('no exportable volumes');
        expect(screen.queryByRole('button', { name: /Storage location/ })).toBeNull();
    });
});
