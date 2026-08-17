import { describe, expect, it, vi } from 'vitest';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { clientUploadLocation, serverFileLocation } from '../../lib/storageLocations';
import type { AudioImportTarget, ImageTransport, Tx16wImportInspection } from '../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import type { JobController } from '../jobs/actions';
import { collectTx16wVolumeOptions, Tx16wImportWorkflow } from './tx16wWorkflow.svelte';

const target = { partitionIndex: 0, volumeName: 'Imported' };
const diskSource: ClientUploadSource = {
    name: 'library.ima',
    type: 'application/octet-stream',
    size: 1_474_560,
    readChunk: vi.fn(),
};

function inspection(): Tx16wImportInspection {
    return {
        schemaVersion: '1.0',
        sourceMembers: ['library.ima'],
        importMode: 'HIERARCHY',
        profile: 'YAMAHA_NATIVE',
        target,
        valid: true,
        counts: { programs: 1, sampleBanks: 1, samples: 1, waveData: 1 },
        objects: {
            programs: [],
            sampleBanks: [],
            samples: [],
            waveData: [],
        },
        notices: [],
    };
}

function sourceTree(): DiskTreeItem[] {
    return [
        {
            id: 'partition-0',
            name: 'Samples',
            kind: 'partition',
            partitionIndex: 0,
            childCount: 1,
            children: [
                {
                    id: 'volume-0',
                    name: 'Imported',
                    kind: 'volume',
                    partitionIndex: 0,
                    childCount: 0,
                },
            ],
        },
    ];
}

describe('Tx16wImportWorkflow', () => {
    it('waits for a target when no volume is selected, then uploads and inspects', async () => {
        const upload = clientUploadLocation({ uploadId: 'upload-1' }, 'DISK_IMAGE', 'library.ima');
        const inspectTx16wDiskSet = vi.fn().mockResolvedValue(inspection());
        const uploadClientFile = vi.fn().mockResolvedValue(upload);
        const workflow = createWorkflow(
            {
                uploadClientFile,
                inspectTx16wDiskSet,
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
            },
            { id: 'partition-0', name: 'Samples', kind: 'partition', partitionIndex: 0, childCount: 1 },
        );

        await workflow.requestDroppedFiles([diskSource]);

        expect(workflow.request?.status).toBe('waiting-target');
        expect(uploadClientFile).not.toHaveBeenCalled();

        await workflow.selectTarget(target);

        expect(uploadClientFile).toHaveBeenCalledWith(
            diskSource,
            'DISK_IMAGE',
            expect.any(Function),
            expect.any(AbortSignal),
        );
        expect(inspectTx16wDiskSet).toHaveBeenCalledWith(12, [upload], target, 'HIERARCHY');
        expect(workflow.request?.status).toBe('ready');
        expect(workflow.request?.inspection?.valid).toBe(true);
    });

    it('commits the inspected disk as one mutation job and releases its upload', async () => {
        const upload = clientUploadLocation({ uploadId: 'upload-2' }, 'DISK_IMAGE', 'library.ima');
        const releaseClientUpload = vi.fn().mockResolvedValue(undefined);
        const startTx16wDiskSetImport = vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' });
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const invalidateSession = vi.fn().mockResolvedValue(undefined);
        const selectWorkspace = vi.fn();
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                inspectTx16wDiskSet: vi.fn().mockResolvedValue(inspection()),
                startTx16wDiskSetImport,
                releaseClientUpload,
            },
            sourceTree()[0].children![0],
            { refreshSession, invalidateSession, selectWorkspace },
        );

        await workflow.requestDroppedFiles([diskSource]);
        await workflow.commit();

        expect(invalidateSession).toHaveBeenCalledWith(12);
        expect(startTx16wDiskSetImport).toHaveBeenCalledWith(12, [upload], target, 'HIERARCHY');
        expect(selectWorkspace).toHaveBeenCalledWith('programs');
        expect(refreshSession).toHaveBeenCalledWith(target);
        expect(releaseClientUpload).toHaveBeenCalledWith(upload);
        expect(workflow.request).toBeNull();
    });

    it('stages every explicitly selected member and can replan as Wave Data only', async () => {
        const secondSource = { ...diskSource, name: 'library_2.ima' };
        const uploads = [
            clientUploadLocation({ uploadId: 'upload-a' }, 'DISK_IMAGE', diskSource.name),
            clientUploadLocation({ uploadId: 'upload-b' }, 'DISK_IMAGE', secondSource.name),
        ];
        const inspectTx16wDiskSet = vi.fn().mockResolvedValue(inspection());
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValueOnce(uploads[0]).mockResolvedValueOnce(uploads[1]),
                inspectTx16wDiskSet,
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
            },
            sourceTree()[0].children![0],
        );

        await workflow.requestDroppedFiles([diskSource, secondSource]);
        await workflow.selectImportMode('WAVE_DATA_ONLY');

        expect(inspectTx16wDiskSet).toHaveBeenLastCalledWith(12, uploads, target, 'WAVE_DATA_ONLY');
        expect(workflow.request?.members.map((member) => member.sourceName)).toEqual(['library.ima', 'library_2.ima']);
    });

    it('refreshes and closes after a submitted job cannot confirm its result', async () => {
        const upload = clientUploadLocation({ uploadId: 'upload-3' }, 'DISK_IMAGE', 'library.ima');
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                inspectTx16wDiskSet: vi.fn().mockResolvedValue(inspection()),
                startTx16wDiskSetImport: vi.fn().mockResolvedValue({ jobId: 23, status: 'queued' }),
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
            },
            sourceTree()[0].children![0],
            {
                refreshSession,
                runJob: async (start) => {
                    await start();
                    throw new Error('Job result violated its declared schema');
                },
            },
        );

        await workflow.requestDroppedFiles([diskSource]);
        await workflow.commit();

        expect(refreshSession).toHaveBeenCalledWith(target);
        expect(workflow.request).toBeNull();
    });

    it('exposes every nested volume as a target without introducing A-series data into the parser model', () => {
        expect(collectTx16wVolumeOptions(sourceTree())).toEqual([
            {
                key: '0:Imported',
                label: 'Samples · Imported',
                target,
            },
        ]);
    });
});

function createWorkflow(
    operations: Partial<ImageTransport>,
    selectedSource: DiskTreeItem,
    overrides: {
        refreshSession?: (preferred: AudioImportTarget) => Promise<void>;
        invalidateSession?: (sessionId: number) => Promise<void>;
        selectWorkspace?: (view: WorkspaceView) => void;
        runJob?: (start: () => Promise<unknown>) => Promise<unknown>;
    } = {},
): Tx16wImportWorkflow {
    const transport = operations as ImageTransport;
    return new Tx16wImportWorkflow({
        transport,
        jobs: {
            run: vi.fn(
                overrides.runJob ??
                    (async (start: () => Promise<unknown>) => {
                        await start();
                        return { jobId: 22, status: 'completed' };
                    }),
            ),
        } as unknown as JobController,
        sessionId: () => 12,
        imageLocation: () => serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
        mutationsAvailable: () => true,
        selectedSource: () => selectedSource,
        sourceItems: sourceTree,
        refreshSession: overrides.refreshSession ?? vi.fn().mockResolvedValue(undefined),
        invalidateSession: overrides.invalidateSession ?? vi.fn().mockResolvedValue(undefined),
        selectWorkspace: overrides.selectWorkspace ?? vi.fn(),
        setStatus: vi.fn(),
        reportTiming: vi.fn(),
    });
}
