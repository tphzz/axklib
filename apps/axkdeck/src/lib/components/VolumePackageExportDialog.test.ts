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
    it('keeps direct native errors in the approved Cancel/Retry state without storage choices', async () => {
        const onretry = vi.fn();
        const oncancel = vi.fn();
        render(VolumePackageExportDialog, {
            props: {
                scopeName: 'A3000 ProSnd Lib',
                inspection,
                desktop: true,
                destinationFlow: 'DIRECT_COMPUTER',
                loading: false,
                picking: false,
                error: 'Download archive cache is unavailable.',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                onretry,
                oncancel,
            },
        });
        expect(screen.queryByRole('button', { name: /Storage location/ })).toBeNull();
        expect(screen.queryByRole('button', { name: /This computer/ })).toBeNull();
        expect(screen.getByRole('alert').textContent).toContain('Download archive cache');
        expect(screen.getByRole('alert').tabIndex).toBe(0);
        await fireEvent.focus(screen.getByRole('button', { name: 'Retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
        expect(onretry).toHaveBeenCalledOnce();
        expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Cancel' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('disables Retry while the approved native source re-inspection state is pending', () => {
        render(VolumePackageExportDialog, {
            props: {
                scopeName: 'A3000 ProSnd Lib',
                inspection,
                desktop: true,
                destinationFlow: 'DIRECT_COMPUTER',
                loading: true,
                picking: false,
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                onretry: vi.fn(),
                oncancel: vi.fn(),
            },
        });
        expect((screen.getByRole('button', { name: 'Retry' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(false);
        expect(screen.getByRole('dialog').getAttribute('aria-busy')).toBe('true');
        expect(screen.queryByRole('button', { name: /Storage location/ })).toBeNull();
    });

    it('shows deterministic package outcomes and both desktop destinations', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(VolumePackageExportDialog, {
            props: {
                scopeName: 'Partition 0',
                inspection,
                desktop: true,
                loading: false,
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
