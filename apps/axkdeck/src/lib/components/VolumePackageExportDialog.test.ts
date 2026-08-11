import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import VolumePackageExportDialog from './VolumePackageExportDialog.svelte';

const inspection = {
    imageId: 'image-1',
    revision: 1,
    sourceMediaKind: 'SFS' as const,
    scopeId: 'partition-0',
    scopeName: 'Partition 0',
    defaultDirectoryName: 'Partition 0 packages',
    volumeCount: 2,
    exportableCount: 1,
    emptyCount: 1,
    volumes: [
        {
            contentId: 'volume-1',
            name: 'Drums',
            displayName: 'Drums',
            partitionIndex: 0,
            volumeDirectoryId: 17,
            objectCount: 4,
            state: 'READY' as const,
            packagePath: 'Drums.axkvol',
        },
        {
            contentId: 'volume-2',
            name: 'Empty',
            displayName: 'Empty',
            partitionIndex: 0,
            volumeDirectoryId: 18,
            objectCount: 0,
            state: 'EMPTY' as const,
            packagePath: null,
        },
    ],
};

describe('VolumePackageExportDialog', () => {
    it('shows deterministic package outcomes and both desktop destinations', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(VolumePackageExportDialog, {
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

        expect(screen.getByText('1 package')).toBeTruthy();
        expect(screen.getByText('1 empty skipped')).toBeTruthy();
        expect(screen.getByText('Drums.axkvol')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('does not offer a destination when every volume is empty', () => {
        render(VolumePackageExportDialog, {
            props: {
                scopeName: 'Partition 0',
                inspection: { ...inspection, exportableCount: 0, emptyCount: 2 },
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

        expect(screen.getByRole('alert').textContent).toContain('no non-empty volumes');
        expect(screen.queryByRole('button', { name: /Storage location/ })).toBeNull();
    });
});
