import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import MediaExportDialog from './MediaExportDialog.svelte';

const item = {
    id: 'partition-0',
    name: 'DRUMS',
    kind: 'partition' as const,
    childCount: 1,
    partitionIndex: 0,
};

describe('MediaExportDialog', () => {
    it('uses the shared destination choices for an admitted CD-ROM export', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(MediaExportDialog, {
            props: {
                request: {
                    item,
                    selection: { format: 'ISO9660', partitionIndex: 0 },
                    inspection: {
                        imageId: 'image-one',
                        revision: 4,
                        format: 'ISO9660',
                        scope: 'PARTITION',
                        partitionIndex: 0,
                        partitionName: 'DRUMS',
                        canExport: true,
                        objectCount: 12,
                        payloadBytes: 2_048,
                        projectedOutputBytes: 65_536,
                        capacityBytes: 700_000_000,
                        volumes: [{ volumeDirectoryId: 17, name: 'KIT', objectCount: 12, payloadBytes: 2_048 }],
                        issues: [],
                        defaultFilename: 'disk_p00_DRUMS.iso',
                    },
                    loading: false,
                    busy: false,
                    jobId: null,
                    progressLabel: '',
                    error: '',
                },
                desktop: true,
                onworkspace,
                onlocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText(/12 objects/)).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('blocks destinations and explains required capacity', () => {
        render(MediaExportDialog, {
            props: {
                request: {
                    item: { ...item, kind: 'volume', volumeDirectoryId: 17 },
                    selection: { format: 'FAT12_FLOPPY', partitionIndex: 0, volumeDirectoryId: 17 },
                    inspection: {
                        imageId: 'image-one',
                        revision: 4,
                        format: 'FAT12_FLOPPY',
                        scope: 'VOLUME',
                        partitionIndex: 0,
                        partitionName: 'DRUMS',
                        canExport: false,
                        objectCount: 12,
                        payloadBytes: 2_000_000,
                        projectedOutputBytes: 1_474_560,
                        capacityBytes: 1_457_664,
                        volumes: [{ volumeDirectoryId: 17, name: 'KIT', objectCount: 12, payloadBytes: 2_000_000 }],
                        issues: [
                            {
                                code: 'floppy_capacity_exceeded',
                                message: 'The volume does not fit on one floppy disk.',
                                blocking: true,
                                requiredBytes: 2_000_000,
                                availableBytes: 1_457_664,
                            },
                        ],
                        defaultFilename: 'disk_p00_KIT.ima',
                    },
                    loading: false,
                    busy: false,
                    jobId: null,
                    progressLabel: '',
                    error: '',
                },
                desktop: true,
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('alert').textContent).toContain('does not fit');
        expect(screen.getByText(/Required 1.9 MiB/)).toBeTruthy();
        expect((screen.getByRole('button', { name: /Storage location/ }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: /This computer/ }) as HTMLButtonElement).disabled).toBe(true);
    });
});
