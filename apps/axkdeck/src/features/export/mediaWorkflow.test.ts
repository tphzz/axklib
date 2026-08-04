import { describe, expect, it, vi } from 'vitest';
import type { ImageTransport } from '../../lib/transport';
import { MediaExportWorkflow } from './mediaWorkflow.svelte';

describe('MediaExportWorkflow', () => {
    it('inspects partitions as CD-ROMs and stable volume directory IDs as floppies', async () => {
        const inspectImageMediaConversion = vi.fn().mockImplementation(async (_sessionId, selection) => ({
            imageId: 'image-one',
            revision: 1,
            format: selection.format,
            scope: selection.format === 'ISO9660' ? 'PARTITION' : 'VOLUME',
            artifactKind: 'IMAGE',
            outputExtension: selection.format === 'ISO9660' ? '.iso' : '.ima',
            floppyImageCount: selection.format === 'ISO9660' ? 0 : 1,
            partitionIndex: selection.partitionIndex,
            partitionName: 'PARTITION 1',
            canExport: true,
            objectCount: 1,
            payloadBytes: 128,
            projectedOutputBytes: 1_474_560,
            capacityBytes: 1_457_664,
            volumes: [],
            issues: [],
            defaultFilename: selection.format === 'ISO9660' ? 'disk_p00_PARTITION_1.iso' : 'disk_p00_KIT.ima',
        }));
        const workflow = new MediaExportWorkflow({
            transport: { inspectImageMediaConversion } as unknown as ImageTransport,
            jobs: {} as never,
            picker: {} as never,
            isDesktop: false,
            sessionId: () => 9,
            setStatus: vi.fn(),
        });

        await workflow.open({
            id: 'partition',
            name: 'PARTITION 1',
            kind: 'partition',
            childCount: 1,
            partitionIndex: 0,
        });
        expect(inspectImageMediaConversion).toHaveBeenLastCalledWith(9, {
            format: 'ISO9660',
            partitionIndex: 0,
        });

        await workflow.open({
            id: 'volume',
            name: 'KIT',
            kind: 'volume',
            childCount: 1,
            partitionIndex: 0,
            volumeDirectoryId: 41,
        });
        expect(inspectImageMediaConversion).toHaveBeenLastCalledWith(9, {
            format: 'FAT12_FLOPPY',
            partitionIndex: 0,
            volumeDirectoryId: 41,
        });
        expect(workflow.request?.inspection?.defaultFilename).toBe('disk_p00_KIT.ima');
    });

    it('uses the inspected ZIP artifact for a multi-floppy workspace destination', async () => {
        const chooseLocation = vi.fn().mockResolvedValue(null);
        const workflow = new MediaExportWorkflow({
            transport: {
                inspectImageMediaConversion: vi.fn().mockResolvedValue({
                    imageId: 'image-one',
                    revision: 1,
                    format: 'FAT12_FLOPPY',
                    scope: 'VOLUME',
                    artifactKind: 'FLOPPY_DISK_SET',
                    outputExtension: '.zip',
                    floppyImageCount: 2,
                    partitionIndex: 0,
                    partitionName: 'PARTITION 1',
                    canExport: true,
                    objectCount: 335,
                    payloadBytes: 268_000,
                    projectedOutputBytes: 2_950_000,
                    capacityBytes: 47_185_920,
                    volumes: [],
                    issues: [],
                    defaultFilename: 'disk_p00_Analog_Update.zip',
                }),
            } as unknown as ImageTransport,
            jobs: {} as never,
            picker: { chooseLocation } as never,
            isDesktop: false,
            sessionId: () => 9,
            setStatus: vi.fn(),
        });

        await workflow.open({
            id: 'volume',
            name: 'Analog Update',
            kind: 'volume',
            childCount: 335,
            partitionIndex: 0,
            volumeDirectoryId: 41,
        });
        await workflow.toWorkspace();

        expect(chooseLocation).toHaveBeenCalledWith(
            'save-file',
            'Export floppy disk set',
            ['zip'],
            'disk_p00_Analog_Update.zip',
            expect.any(Object),
        );
    });
});
