import { describe, expect, it, vi } from 'vitest';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { clientUploadLocation, serverFileLocation } from '../../lib/storageLocations';
import type { AudioImportTarget, ImageTransport } from '../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import type { JobController } from '../jobs/actions';
import { collectTx16wVolumeOptions, Tx16wImportWorkflow } from './tx16wWorkflow.svelte';
import type { ConversionPlan, CompanionSourceInput, CompanionSourceResolution } from '../../lib/conversions';
import type { InputFileLocation } from '../../lib/storageLocations';
import type { PickerController } from '../dialogs/picker';
import { fitsCapacity, inspectFitsCapacity } from '../../test/samplerCapacityFixture';

const target = { partitionIndex: 0, volumeName: 'Imported' };
const diskSource: ClientUploadSource = {
    name: 'library.ima',
    type: 'application/octet-stream',
    size: 1_474_560,
    readChunk: vi.fn(),
};

function selectedMembers(sources: InputFileLocation[]): CompanionSourceResolution['members'] {
    return sources.map((source) => ({
        source:
            source.kind === 'server-file'
                ? { kind: 'FILE', file: source.reference }
                : { kind: 'UPLOAD', upload: source.reference },
        name: source.displayName.split('/').at(-1) ?? source.displayName,
        origin: 'SELECTED',
    }));
}

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
    it.each([1, 2])('locks the old plan immediately while removing from %i sources', async (count) => {
        let finish!: () => void;
        const releaseClientUpload = vi.fn(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        const executeConversion = vi.fn();
        const workflow = createWorkflow(
            {
                uploadClientFile: vi.fn(async (source: ClientUploadSource) =>
                    clientUploadLocation({ uploadId: source.name }, 'DISK_IMAGE', source.name),
                ),
                releaseClientUpload,
                executeConversion,
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles(
            Array.from({ length: count }, (_, index) => ({ ...diskSource, name: `disk-${index}.img` })),
        );
        const pending = workflow.removeMember(workflow.request!.members[0].id);
        expect(workflow.request!.status).toBe('inspecting');
        await workflow.commit();
        expect(executeConversion).not.toHaveBeenCalled();
        await vi.waitFor(() => expect(releaseClientUpload).toHaveBeenCalledOnce());
        finish();
        await pending;
        expect(workflow.request!.members).toHaveLength(count - 1);
    });

    it('keeps failed last-source cleanup visible without retaining an executable plan', async () => {
        const executeConversion = vi.fn();
        const workflow = createWorkflow(
            {
                uploadClientFile: vi
                    .fn()
                    .mockResolvedValue(clientUploadLocation({ uploadId: 'seed' }, 'DISK_IMAGE', diskSource.name)),
                releaseClientUpload: vi.fn().mockRejectedValue(new Error('Upload cleanup failed')),
                executeConversion,
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([diskSource]);
        await workflow.removeMember(workflow.request!.members[0].id);
        expect(workflow.request!.error).toBe('Upload cleanup failed');
        expect(workflow.request!.plan).toBeNull();
        await workflow.commit();
        expect(executeConversion).not.toHaveBeenCalled();
    });

    it('joins removal cleanup on Cancel and ignores its late membership result', async () => {
        let finish!: () => void;
        const releaseClientUpload = vi
            .fn()
            .mockImplementationOnce(
                () =>
                    new Promise<void>((resolve) => {
                        finish = resolve;
                    }),
            )
            .mockResolvedValue(undefined);
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const workflow = createWorkflow(
            {
                uploadClientFile: vi
                    .fn()
                    .mockResolvedValue(clientUploadLocation({ uploadId: 'seed' }, 'DISK_IMAGE', diskSource.name)),
                releaseClientUpload,
                prepareConversion,
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([diskSource]);
        const oldRequest = workflow.request!;
        const removal = workflow.removeMember(oldRequest.members[0].id);
        await vi.waitFor(() => expect(releaseClientUpload).toHaveBeenCalledOnce());
        const closing = workflow.close();
        await Promise.resolve();
        expect(workflow.request).toBe(oldRequest);
        finish();
        await Promise.all([removal, closing]);
        expect(oldRequest.members).toHaveLength(1);
        expect(releaseClientUpload).toHaveBeenCalledOnce();
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'root', relativePath: 'new.img' })]);
        expect(workflow.request!.members[0].sourceName).toBe('new.img');
        expect(prepareConversion).toHaveBeenCalledTimes(2);
        await workflow.close();
    });

    it('keeps removed canonical native members excluded until explicitly re-added', async () => {
        const companion = { ...diskSource, name: 'second.img', identity: 'native:second' };
        const seed = { ...diskSource, identity: 'native:first', nearbySources: vi.fn().mockResolvedValue([companion]) };
        let uploadId = 0;
        const uploadClientFile = vi.fn(async (source: ClientUploadSource) =>
            clientUploadLocation({ uploadId: `upload-${++uploadId}` }, 'DISK_IMAGE', source.name),
        );
        const releaseClientUpload = vi.fn().mockResolvedValue(undefined);
        const workflow = createWorkflow(
            {
                connectionMode: 'local',
                uploadClientFile,
                releaseClientUpload,
                resolveCompanionSources: vi.fn(
                    async (input: CompanionSourceInput): Promise<CompanionSourceResolution> => ({
                        members: [
                            ...selectedMembers(input.sources),
                            ...selectedMembers(input.candidates ?? []).map((member) => ({
                                ...member,
                                origin: 'AUTO_ADDED' as const,
                            })),
                        ],
                        requirements:
                            input.sources.length > 1 || input.candidates?.length
                                ? []
                                : [{ name: 'Wave.W01', state: 'MISSING' }],
                    }),
                ),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([seed]);
        const second = workflow.request!.members[1];
        expect(second.origin).toBe('AUTO_ADDED');
        await workflow.removeMember(second.id);
        expect(workflow.request!.members.map((member) => member.sourceName)).toEqual([seed.name]);
        expect(uploadClientFile).toHaveBeenCalledTimes(2);
        await workflow.addFiles([{ ...companion, name: 'same-file-readded.img' }]);
        expect(workflow.request!.members.map((member) => member.origin)).toEqual(['SELECTED', 'SELECTED']);
        expect(uploadClientFile).toHaveBeenCalledTimes(3);
        await workflow.close();
        expect(releaseClientUpload).toHaveBeenCalledTimes(3);
    });

    it('deduplicates source identity rather than names or read callbacks, including reactive members', async () => {
        let id = 0;
        const uploadClientFile = vi.fn(async (source: ClientUploadSource) =>
            clientUploadLocation({ uploadId: `${++id}` }, 'DISK_IMAGE', source.name),
        );
        const workflow = createWorkflow(
            {
                uploadClientFile,
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([diskSource, diskSource]);
        await workflow.addFiles([diskSource]);
        expect(uploadClientFile).toHaveBeenCalledOnce();
        await workflow.addFiles([{ ...diskSource }]);
        expect(workflow.request!.members).toHaveLength(2);
        expect(uploadClientFile).toHaveBeenCalledTimes(2);
        await workflow.close();
    });

    it('blocks execution after an incomplete native search and retries explicitly', async () => {
        const nearbySources = vi
            .fn()
            .mockRejectedValueOnce(new Error('Directory scan exceeds its limit'))
            .mockResolvedValueOnce([]);
        const executeConversion = vi.fn();
        const workflow = createWorkflow(
            {
                connectionMode: 'local',
                executeConversion,
                uploadClientFile: vi
                    .fn()
                    .mockResolvedValue(clientUploadLocation({ uploadId: 'seed' }, 'DISK_IMAGE', diskSource.name)),
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
                resolveCompanionSources: vi.fn(
                    async (input: CompanionSourceInput): Promise<CompanionSourceResolution> => ({
                        members: selectedMembers(input.sources),
                        requirements: [{ name: 'Wave.W01', state: 'MISSING' }],
                    }),
                ),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([{ ...diskSource, nearbySources }]);
        expect(workflow.request!.discovery.error).toContain('limit');
        await workflow.commit();
        expect(executeConversion).not.toHaveBeenCalled();
        await workflow.reprepare();
        expect(nearbySources).toHaveBeenCalledTimes(2);
        expect(workflow.request!.discovery.error).toBe('');
        await workflow.close();
    });

    it('clears consent and blocks execution when releasing an obsolete plan fails', async () => {
        const prepared = inspection();
        prepared.report!.issues = [
            {
                id: 'loss',
                impact: 'LOSS',
                message: 'Resampled',
                code: 'resample',
                sourceObject: 'wave',
                sourceParameter: '',
                targetObject: '',
                targetParameter: '',
            },
        ];
        prepared.report!.issueCount = 1;
        const releaseConversion = vi.fn().mockRejectedValue(new Error('Plan release failed'));
        const executeConversion = vi.fn();
        const workflow = createWorkflow(
            { releaseConversion, executeConversion, prepareConversion: vi.fn().mockResolvedValue(prepared) },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'root', relativePath: 'disk.img' })]);
        workflow.request!.acknowledgedIssueIds = ['loss'];
        await workflow.selectFidelity('STRICT');
        expect(workflow.request!.acknowledgedIssueIds).toEqual([]);
        expect(workflow.request!.error).toContain('release failed');
        await workflow.commit();
        expect(executeConversion).not.toHaveBeenCalled();
        releaseConversion.mockResolvedValue(undefined);
        await workflow.close();
    });

    it('keeps an empty source dialog open and rejects oversized uploads before staging', async () => {
        const uploadClientFile = vi.fn();
        const workflow = createWorkflow({ uploadClientFile }, sourceTree()[0]);
        await workflow.requestDroppedFiles([{ ...diskSource, size: 4 * 1024 * 1024 + 1 }]);
        expect(workflow.request).toBeNull();
        expect(uploadClientFile).not.toHaveBeenCalled();
        await workflow.requestDroppedFiles([serverFileLocation({ rootId: 'root', relativePath: 'disk.img' })], null);
        await workflow.removeMember(workflow.request!.members[0].id);
        expect(workflow.request!.members).toEqual([]);
        expect(workflow.request!.status).toBe('waiting-target');
        await workflow.close();
    });

    it('discovers server-workspace companions on remote servers and marks their origin', async () => {
        const first = serverFileLocation({ rootId: 'remote', relativePath: 'set/first.img' });
        const second = serverFileLocation({ rootId: 'remote', relativePath: 'set/second.img' }, 'second.img');
        const resolveCompanionSources = vi.fn().mockResolvedValue({
            members: [
                { source: { kind: 'FILE', file: first.reference }, name: 'first.img', origin: 'SELECTED' },
                { source: { kind: 'FILE', file: second.reference }, name: 'second.img', origin: 'AUTO_ADDED' },
            ],
            requirements: [],
        });
        const prepareConversion = vi.fn().mockResolvedValue(inspection());
        const workflow = createWorkflow(
            { connectionMode: 'remote', resolveCompanionSources, prepareConversion },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([first]);
        expect(resolveCompanionSources).toHaveBeenCalledWith(
            expect.objectContaining({ sources: [first], searchNearby: true }),
            expect.any(AbortSignal),
        );
        expect(workflow.request?.members.map((member) => member.sourceName)).toEqual(['first.img', 'second.img']);
        expect(workflow.request?.members[1].origin).toBe('AUTO_ADDED');
        expect(prepareConversion).toHaveBeenCalledWith(
            expect.objectContaining({ source: { kind: 'SOURCES', sources: [first, second] } }),
            expect.any(AbortSignal),
        );
    });

    it('uploads native candidates only locally, releases nonmembers and does not rescan for fidelity changes', async () => {
        const companion = { ...diskSource, name: 'second.img', identity: 'native:second' };
        const unrelated = { ...diskSource, name: 'other.img', identity: 'native:other' };
        const nearbySources = vi.fn().mockResolvedValue([companion, unrelated]);
        const seed = { ...diskSource, identity: 'native:first', nearbySources };
        const uploadClientFile = vi.fn(async (source: ClientUploadSource) =>
            clientUploadLocation({ uploadId: source.name }, 'DISK_IMAGE', source.name),
        );
        const releaseClientUpload = vi.fn().mockResolvedValue(undefined);
        const resolveCompanionSources = vi.fn(
            async (input: CompanionSourceInput): Promise<CompanionSourceResolution> => ({
                members: [
                    ...selectedMembers(input.sources),
                    ...(input.candidates?.length
                        ? [
                              {
                                  source: { kind: 'UPLOAD' as const, upload: { uploadId: companion.name } },
                                  name: companion.name,
                                  origin: 'AUTO_ADDED' as const,
                              },
                          ]
                        : []),
                ],
                requirements: input.candidates?.length ? [] : [{ name: 'Wave.W01', state: 'MISSING' }],
            }),
        );
        const workflow = createWorkflow(
            {
                connectionMode: 'local',
                uploadClientFile,
                releaseClientUpload,
                resolveCompanionSources,
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([seed]);
        expect(nearbySources).toHaveBeenCalledOnce();
        expect(workflow.request?.members.map((member) => member.sourceName)).toEqual([seed.name, companion.name]);
        expect(releaseClientUpload).toHaveBeenCalledWith(
            clientUploadLocation({ uploadId: unrelated.name }, 'DISK_IMAGE', unrelated.name),
        );
        await workflow.selectFidelity('STRICT');
        expect(nearbySources).toHaveBeenCalledOnce();
        expect(resolveCompanionSources).toHaveBeenCalledTimes(2);
        await workflow.close();
    });

    it('never scans native local siblings for a desktop-to-remote upload', async () => {
        const nearbySources = vi.fn();
        const workflow = createWorkflow(
            {
                connectionMode: 'remote',
                uploadClientFile: vi
                    .fn()
                    .mockResolvedValue(clientUploadLocation({ uploadId: 'first' }, 'DISK_IMAGE', diskSource.name)),
                releaseClientUpload: vi.fn().mockResolvedValue(undefined),
                resolveCompanionSources: vi.fn().mockResolvedValue({
                    members: [
                        {
                            source: { kind: 'UPLOAD', upload: { uploadId: 'first' } },
                            name: diskSource.name,
                            origin: 'SELECTED',
                        },
                    ],
                    requirements: [{ name: 'Wave.W01', state: 'MISSING' }],
                }),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        await workflow.requestDroppedFiles([{ ...diskSource, nearbySources }]);
        expect(nearbySources).not.toHaveBeenCalled();
        expect(workflow.request?.discovery.state).toBe('manual');
        await workflow.close();
    });

    it('releases a candidate upload that finishes after cancellation without publishing membership', async () => {
        let finishUpload!: (source: ReturnType<typeof clientUploadLocation>) => void;
        const candidate = { ...diskSource, name: 'second.img', identity: 'native:second' };
        const releaseClientUpload = vi.fn().mockResolvedValue(undefined);
        const uploadClientFile = vi
            .fn()
            .mockResolvedValueOnce(clientUploadLocation({ uploadId: 'first' }, 'DISK_IMAGE', diskSource.name))
            .mockImplementationOnce(
                () =>
                    new Promise((resolve) => {
                        finishUpload = resolve;
                    }),
            );
        const workflow = createWorkflow(
            {
                connectionMode: 'local',
                uploadClientFile,
                releaseClientUpload,
                resolveCompanionSources: vi.fn().mockResolvedValue({
                    members: [
                        {
                            source: { kind: 'UPLOAD', upload: { uploadId: 'first' } },
                            name: diskSource.name,
                            origin: 'SELECTED',
                        },
                    ],
                    requirements: [{ name: 'Wave.W01', state: 'MISSING' }],
                }),
                prepareConversion: vi.fn().mockResolvedValue(inspection()),
            },
            sourceTree()[0].children![0],
        );
        const pending = workflow.requestDroppedFiles([
            { ...diskSource, identity: 'native:first', nearbySources: vi.fn().mockResolvedValue([candidate]) },
        ]);
        await vi.waitFor(() => expect(uploadClientFile).toHaveBeenCalledTimes(2));
        await workflow.close();
        const late = clientUploadLocation({ uploadId: 'late' }, 'DISK_IMAGE', candidate.name);
        finishUpload(late);
        await pending;
        expect(releaseClientUpload).toHaveBeenCalledWith(late);
        expect(workflow.request).toBeNull();
    });

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
        connectionMode: 'local',
        resolveCompanionSources: vi.fn(async (input: CompanionSourceInput): Promise<CompanionSourceResolution> => ({
            members: selectedMembers(input.sources),
            requirements: [],
        })),
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
