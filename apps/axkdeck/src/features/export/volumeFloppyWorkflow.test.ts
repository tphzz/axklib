import { describe, expect, it, vi } from 'vitest';
import { serverDirectoryLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import { VolumeFloppyExportWorkflow } from './volumeFloppyWorkflow.svelte';

const inspection = {
    imageId: 'image-one',
    revision: 4,
    sourceMediaKind: 'SFS' as const,
    scopeId: 'partition-one',
    scopeName: 'Partition 1',
    defaultDirectoryName: 'Partition 1 floppies',
    volumeCount: 3,
    exportableCount: 1,
    emptyCount: 1,
    blockedCount: 1,
    totalFloppyImageCount: 2,
    projectedDiskBytes: 2_949_120,
    volumes: [],
};

describe('VolumeFloppyExportWorkflow', () => {
    it('inspects one exact partition and keeps per-volume exceptions in the root report', async () => {
        const inspect = vi.fn().mockResolvedValue(inspection);
        const start = vi.fn().mockResolvedValue({ kind: 'images.volume_floppy_export', status: 'queued' });
        const run = vi.fn().mockImplementation(async (begin: () => Promise<unknown>) => {
            await begin();
            return {
                status: 'completed',
                result: {
                    imageId: 'image-one',
                    revision: 4,
                    scopeId: 'partition-one',
                    volumeCount: 3,
                    exportedCount: 1,
                    skippedCount: 1,
                    blockedCount: 1,
                    failedCount: 0,
                    floppyImageCount: 2,
                    diskBytes: 2_949_120,
                    reportPath: 'volume-floppies.axklib.json',
                    volumes: [],
                    destination: 'WORKSPACE',
                    output: { rootId: 'workspace', relativePath: 'exports/Partition 1 floppies' },
                    download: null,
                },
            };
        });
        const chooseLocation = vi.fn().mockResolvedValue(
            serverDirectoryLocation({
                rootId: 'workspace',
                relativePath: 'exports/Partition 1 floppies',
            }),
        );
        const setStatus = vi.fn();
        const workflow = new VolumeFloppyExportWorkflow({
            transport: {
                inspectImageVolumeFloppyExport: inspect,
                startImageVolumeFloppyExport: start,
            } as unknown as ImageTransport,
            jobs: { run } as never,
            picker: { chooseLocation } as never,
            isDesktop: false,
            sessionId: () => 12,
            setStatus,
        });
        const scope = {
            id: 'partition-one',
            name: 'Partition 1',
            kind: 'partition' as const,
            childCount: 3,
            partitionIndex: 0,
        };

        await workflow.open(scope);
        expect(inspect).toHaveBeenCalledWith(12, 'partition-one');
        await workflow.toWorkspace();

        expect(chooseLocation).toHaveBeenCalledWith(
            'save-directory',
            'Export volumes to floppies',
            [],
            'Partition 1 floppies',
            expect.objectContaining({ parentDialog: 'volume-floppy-export', requireWritableDirectory: true }),
        );
        expect(start).toHaveBeenCalledWith(12, 'partition-one', {
            kind: 'WORKSPACE',
            output: { rootId: 'workspace', relativePath: 'exports/Partition 1 floppies' },
        });
        expect(setStatus).toHaveBeenLastCalledWith('Exported 1 volume to 2 floppy images; 2 recorded in the report');
        expect(workflow.request).toBeNull();
    });
});
