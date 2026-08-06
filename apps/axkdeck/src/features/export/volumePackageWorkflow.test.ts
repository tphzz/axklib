import { describe, expect, it, vi } from 'vitest';
import { serverDirectoryLocation } from '../../lib/storageLocations';
import type { ImageTransport } from '../../lib/transport';
import { VolumePackageExportWorkflow } from './volumePackageWorkflow.svelte';

const inspection = {
    imageId: 'image-one',
    revision: 4,
    sourceMediaKind: 'SFS' as const,
    scopeId: 'partition-one',
    scopeName: 'Partition 1',
    defaultDirectoryName: 'Partition 1 packages',
    volumeCount: 2,
    exportableCount: 1,
    emptyCount: 1,
    volumes: [],
};

describe('VolumePackageExportWorkflow', () => {
    it('inspects one exact scope and publishes the batch to a new workspace directory', async () => {
        const inspect = vi.fn().mockResolvedValue(inspection);
        const start = vi.fn().mockResolvedValue({ kind: 'images.volume_package_export', status: 'queued' });
        const run = vi.fn().mockImplementation(async (begin: () => Promise<unknown>) => {
            await begin();
            return {
                status: 'completed',
                result: {
                    imageId: 'image-one',
                    revision: 4,
                    scopeId: 'partition-one',
                    volumeCount: 2,
                    exportedCount: 1,
                    skippedCount: 1,
                    failedCount: 0,
                    reportPath: 'volume-packages.axklib.json',
                    volumes: [],
                    destination: 'WORKSPACE',
                    output: { rootId: 'workspace', relativePath: 'exports/Partition 1 packages' },
                    download: null,
                },
            };
        });
        const chooseLocation = vi.fn().mockResolvedValue(
            serverDirectoryLocation({
                rootId: 'workspace',
                relativePath: 'exports/Partition 1 packages',
            }),
        );
        const setStatus = vi.fn();
        const workflow = new VolumePackageExportWorkflow({
            transport: {
                inspectImageVolumePackageExport: inspect,
                startImageVolumePackageExport: start,
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
            childCount: 2,
            partitionIndex: 0,
        };

        await workflow.open(scope);
        expect(inspect).toHaveBeenCalledWith(12, 'partition-one');
        await workflow.toWorkspace();

        expect(chooseLocation).toHaveBeenCalledWith(
            'save-directory',
            'Export volume packages',
            [],
            'Partition 1 packages',
            expect.objectContaining({ parentDialog: 'volume-package-export', requireWritableDirectory: true }),
        );
        expect(start).toHaveBeenCalledWith(12, 'partition-one', {
            kind: 'WORKSPACE',
            output: { rootId: 'workspace', relativePath: 'exports/Partition 1 packages' },
        });
        expect(setStatus).toHaveBeenLastCalledWith('Exported 1 volume package; 1 recorded in the report');
        expect(workflow.request).toBeNull();
    });
});
