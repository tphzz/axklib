import { describe, expect, it, vi } from 'vitest';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { clientUploadLocation, serverFileLocation } from '../../lib/storageLocations';
import type { AudioImportTarget, ImageTransport } from '../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import type { JobController } from '../jobs/actions';
import { collectTx16wVolumeOptions, Tx16wImportWorkflow } from './tx16wWorkflow.svelte';
import type { ConversionPlan } from '../../lib/conversions';
import type { PickerController } from '../dialogs/picker';
import { fitsCapacity, inspectFitsCapacity } from '../../test/samplerCapacityFixture';

const target = { partitionIndex: 0, volumeName: 'Imported' };
const diskSource: ClientUploadSource = {
    name: 'library.ima',
    type: 'application/octet-stream',
    size: 1_474_560,
    readChunk: vi.fn(),
};

function inspection(): ConversionPlan {
    return {
        planId: 'conversion-1',
        state: 'READY',
        expiresAt: Date.now() + 900000,
        jobId: null,
        report: {
            profileId: 'yamaha.tx16w-to-a-series',
            fidelity: 'BEST',
            executable: true,
            actionCount: 1,
            issueCount: 0,
            offset: 0,
            actions: [{ kind: 'wave', name: 'WAVE', action: 'create', sourceObject: 'wave-1', details: [] }],
            issues: [],
            capacity: fitsCapacity,
            destination: { imageId: 'image-1', documentId: 'doc-1', expectedRevision: 1 },
        },
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
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const uploadClientFile = vi.fn().mockResolvedValue(upload);
        const workflow = createWorkflow(
            {
                uploadClientFile,
                prepareConversion,
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
        expect(prepareConversion).toHaveBeenCalledWith(
            expect.objectContaining({
                destinationSessionId: 12,
                source: { kind: 'SOURCES', sources: [upload] },
                selection: target,
                options: { mode: 'HIERARCHY' },
            }),
            expect.any(AbortSignal),
        );
        expect(workflow.request?.status).toBe('ready');
        expect(workflow.request?.plan?.report?.executable).toBe(true);
    });

    it('commits the inspected disk as one mutation job and releases its upload', async () => {
        const upload = clientUploadLocation({ uploadId: 'upload-2' }, 'DISK_IMAGE', 'library.ima');
        const releaseClientUpload = vi.fn().mockResolvedValue(undefined);
        const executeConversion = vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' });
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const invalidateSession = vi.fn().mockResolvedValue(undefined);
        const selectWorkspace = vi.fn();
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion,
                releaseClientUpload,
            },
            sourceTree()[0].children![0],
            { refreshSession, invalidateSession, selectWorkspace },
        );

        await workflow.requestDroppedFiles([diskSource]);
        await workflow.commit();

        expect(invalidateSession).toHaveBeenCalledWith(12);
        expect(executeConversion).toHaveBeenCalledWith(expect.objectContaining({ planId: 'conversion-1' }), []);
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
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValueOnce(uploads[0]).mockResolvedValueOnce(uploads[1]),
                prepareConversion,
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
            },
            sourceTree()[0].children![0],
        );

        await workflow.requestDroppedFiles([diskSource, secondSource]);
        await workflow.selectImportMode('WAVE_DATA_ONLY');

        expect(prepareConversion).toHaveBeenLastCalledWith(
            expect.objectContaining({
                source: { kind: 'SOURCES', sources: uploads },
                options: { mode: 'WAVE_DATA_ONLY' },
            }),
            expect.any(AbortSignal),
        );
        expect(workflow.request?.members.map((member) => member.sourceName)).toEqual(['library.ima', 'library_2.ima']);
    });

    it('retains the dialog until upload cleanup finishes', async () => {
        let release!: () => void;
        const upload = clientUploadLocation({ uploadId: 'cleanup' }, 'DISK_IMAGE', 'library.ima');
        const releaseClientUpload = vi.fn(
            () =>
                new Promise<void>((resolve) => {
                    release = resolve;
                }),
        );
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion: vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' }),
                releaseClientUpload,
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([diskSource]);
        const pending = workflow.commit();
        await vi.waitFor(() => expect(releaseClientUpload).toHaveBeenCalled());
        expect(workflow.request).not.toBeNull();
        release();
        await pending;
        expect(workflow.request).toBeNull();
    });

    it('clears a selected target without retaining a stale import destination', async () => {
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const workflow = createWorkflow({ prepareConversion }, sourceTree()[0].children![0]);
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'workspace', relativePath: 'library.img' })]);
        await workflow.selectTarget(null);
        expect(workflow.request?.target).toBeNull();
        expect(workflow.request?.plan).toBeNull();
        expect(workflow.request?.status).toBe('waiting-target');
        expect(prepareConversion).toHaveBeenCalledTimes(1);
    });

    it('recovers a committed refresh failure without another TX16W mutation', async () => {
        const executeConversion = vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' });
        const refreshSession = vi.fn().mockRejectedValueOnce(new Error('Offline')).mockResolvedValue(undefined);
        const workflow = createWorkflow(
            {
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion,
            },
            sourceTree()[0].children![0],
            { refreshSession },
        );
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'workspace', relativePath: 'library.img' })]);
        await workflow.commit();
        expect(workflow.completion.phase).toBe('refresh-failed');
        expect(workflow.request).not.toBeNull();
        await workflow.commit();
        await workflow.recoverCompletion();
        expect(workflow.request).toBeNull();
        expect(executeConversion).toHaveBeenCalledTimes(1);
        expect(refreshSession).toHaveBeenCalledTimes(2);
    });

    it('retains an unconfirmed job for status recovery without resubmitting', async () => {
        const upload = clientUploadLocation({ uploadId: 'upload-3' }, 'DISK_IMAGE', 'library.ima');
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion: vi.fn().mockResolvedValue({ jobId: 23, status: 'queued' }),
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

        expect(refreshSession).not.toHaveBeenCalled();
        expect(workflow.request).not.toBeNull();
        expect(workflow.completion.phase).toBe('unconfirmed');
        expect(workflow.completion.canCheck).toBe(true);
    });

    it('retries failed upload cleanup through Refresh without another write', async () => {
        const upload = clientUploadLocation({ uploadId: 'cleanup-failed' }, 'DISK_IMAGE', 'library.ima');
        const releaseClientUpload = vi
            .fn()
            .mockRejectedValueOnce(new Error('Cleanup offline'))
            .mockResolvedValue(undefined);
        const executeConversion = vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' });
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn().mockResolvedValue(upload),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion,
                releaseClientUpload,
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([diskSource]);
        await workflow.commit();
        expect(workflow.completion.phase).toBe('refresh-failed');
        expect(workflow.request?.members[0].upload).toEqual(upload);
        await workflow.commit();
        await workflow.recoverCompletion();
        expect(workflow.request).toBeNull();
        expect(executeConversion).toHaveBeenCalledTimes(1);
        expect(releaseClientUpload).toHaveBeenCalledTimes(2);
    });

    it('converts an open native document through an owned target session and closes only that target', async () => {
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const opened = {
            sessionId: 99,
            documentId: 'a-series',
            documents: [{ id: 'a-series', formatId: 'yamaha.a-series' }],
            volumeMutationsAvailable: true,
            tree: sourceTree(),
        };
        const closeImage = vi.fn().mockResolvedValue(undefined);
        const refreshImage = vi.fn().mockResolvedValue(opened);
        const refreshSession = vi.fn();
        const invalidateSession = vi.fn();
        const workflow = createWorkflow(
            {
                openImage: vi.fn().mockResolvedValue(opened),
                closeImage,
                refreshImage,
                prepareConversion,
                executeConversion: vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' }),
            },
            sourceTree()[0],
            {
                refreshSession,
                invalidateSession,
                picker: {
                    chooseLocation: vi
                        .fn()
                        .mockResolvedValue(serverFileLocation({ rootId: 'root', relativePath: 'target.hds' })),
                } as unknown as PickerController,
            },
        );
        await workflow.requestNativeDocument(12, 'tx16w', 7);
        expect(workflow.volumeOptions()).toHaveLength(1);
        await workflow.selectTarget(target);
        expect(prepareConversion).toHaveBeenCalledWith(
            expect.objectContaining({
                destinationSessionId: 99,
                source: { kind: 'DOCUMENT', sessionId: 12, documentId: 'tx16w', expectedRevision: 7 },
            }),
            expect.any(AbortSignal),
        );
        await workflow.commit();
        expect(refreshImage).toHaveBeenCalledWith(99);
        expect(closeImage).toHaveBeenCalledWith(99);
        expect(closeImage).not.toHaveBeenCalledWith(12);
        expect(refreshSession).not.toHaveBeenCalled();
        expect(invalidateSession).not.toHaveBeenCalled();
        expect(workflow.request).toBeNull();
    });

    it('recovers an execution identity after a lost response without submitting again', async () => {
        const executeConversion = vi.fn().mockRejectedValue(new TypeError('Connection lost'));
        const conversionExecution = vi.fn().mockResolvedValue({ jobId: 77, status: 'running' });
        const waitForJob = vi.fn().mockResolvedValue({ jobId: 77, status: 'completed' });
        const workflow = createWorkflow(
            {
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
                executeConversion,
                conversionExecution,
                waitForJob,
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'root', relativePath: 'source.ima' })]);
        await workflow.commit();
        expect(workflow.completion.phase).toBe('unconfirmed');
        await workflow.close();
        expect(workflow.request).not.toBeNull();
        await workflow.recoverCompletion();
        expect(conversionExecution).toHaveBeenCalledWith('conversion-1');
        expect(waitForJob).toHaveBeenCalledWith(77, expect.any(Function));
        expect(executeConversion).toHaveBeenCalledTimes(1);
        expect(workflow.request).toBeNull();
    });

    it('exposes every nested volume as a target without introducing A-series data into the parser model', () => {
        expect(collectTx16wVolumeOptions(sourceTree())).toEqual([
            {
                key: '0:Imported',
                label: 'Samples · Imported',
                partitionName: 'Samples',
                target,
            },
        ]);
    });

    it('requires explicit loss acknowledgement and releases a plan when fidelity changes', async () => {
        const prepared = inspection();
        prepared.report!.issueCount = 1;
        prepared.report!.issues = [
            {
                id: 'loss',
                code: 'rate',
                impact: 'LOSS',
                message: 'Resampled',
                sourceObject: 'wave-1',
                sourceParameter: 'rate',
                targetObject: 'WAVE',
                targetParameter: 'rate',
            },
        ];
        const executeConversion = vi.fn().mockResolvedValue({ jobId: 22, status: 'queued' });
        const releaseConversion = vi.fn().mockResolvedValue(undefined);
        const prepareConversion = vi.fn().mockResolvedValue(prepared);
        const workflow = createWorkflow(
            { prepareConversion, executeConversion, releaseConversion },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'root', relativePath: 'disk.ima' })]);
        await workflow.commit();
        expect(executeConversion).not.toHaveBeenCalled();
        await workflow.selectFidelity('STRICT');
        expect(releaseConversion).toHaveBeenCalledWith('conversion-1');
        expect(prepareConversion).toHaveBeenLastCalledWith(
            expect.objectContaining({ fidelity: 'STRICT' }),
            expect.any(AbortSignal),
        );
    });

    it('closes a destination opened after disposal without replacing the source workspace', async () => {
        let resolve!: (opened: unknown) => void;
        const closeImage = vi.fn().mockResolvedValue(undefined);
        const openImage = vi.fn(
            () =>
                new Promise((done) => {
                    resolve = done;
                }),
        );
        const workflow = createWorkflow({ openImage, closeImage } as Partial<ImageTransport>, sourceTree()[0], {
            picker: {
                chooseLocation: vi
                    .fn()
                    .mockResolvedValue(serverFileLocation({ rootId: 'root', relativePath: 'target.hds' })),
            } as unknown as PickerController,
        });
        const pending = workflow.requestNativeDocument(12, 'tx16w', 7);
        await vi.waitFor(() => expect(openImage).toHaveBeenCalled());
        await workflow.dispose();
        resolve({ sessionId: 99 });
        await pending;
        expect(closeImage).toHaveBeenCalledWith(99);
        expect(closeImage).not.toHaveBeenCalledWith(12);
        expect(workflow.request).toBeNull();
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
        picker?: PickerController;
    } = {},
): Tx16wImportWorkflow {
    const transport = {
        releaseConversion: vi.fn().mockResolvedValue(undefined),
        inspectImportCapacity: vi.fn(inspectFitsCapacity),
        ...operations,
    } as ImageTransport;
    return new Tx16wImportWorkflow({
        transport,
        picker: overrides.picker,
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
