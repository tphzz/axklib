import { describe, expect, it, vi } from 'vitest';
import { AxklibApiError } from '../../lib/httpErrors';
import type { ImageLocation } from '../../lib/storageLocations';
import type { ImageTransport, OpenedImage } from '../../lib/transport';
import { PickerController } from '../dialogs/picker';
import { ImageSessionWorkflow } from './workflow.svelte';

const location: ImageLocation = {
    kind: 'server-file',
    reference: { rootId: 'root', relativePath: 'test-disk.hds' },
    displayName: 'test-disk.hds',
};

function opened(sessionId: number): OpenedImage {
    const volume = {
        id: `volume-${sessionId}`,
        name: 'newvolume',
        kind: 'volume' as const,
        childCount: 0,
        partitionIndex: 0,
    };
    return {
        sessionId,
        allocationInspectionAvailable: true,
        revision: 1,
        companionSources: [],
        floppySet: null,
        tree: [
            {
                id: `disk-${sessionId}`,
                name: 'test-disk.hds',
                kind: 'disk',
                childCount: 1,
                children: [volume],
            },
        ],
        validation: {
            valid: true,
            issueCount: 0,
            errorCount: 0,
            warningCount: 0,
            objectCount: 49,
            relationshipCount: 48,
        },
        objects: [],
        objectTotalCount: 0,
        initialVolume: volume,
        volumeMutationsAvailable: true,
        partitionMutationsAvailable: true,
        objectRenameAvailable: true,
        objectDeletionAvailable: true,
        waveDataCleanupAvailable: true,
        programGenerationAvailable: true,
        programAssignmentCleanupAvailable: true,
        packageImportAvailable: true,
        packageExportAvailable: true,
        volumePackageExportAvailable: true,
        volumeFloppyExportAvailable: true,
        audioExportAvailable: true,
        sequenceExportAvailable: true,
        mediaConversionAvailable: true,
        extentLayoutRepairAvailable: true,
        format: 'sfs',
    };
}

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
    let resolve!: (value: T) => void;
    const promise = new Promise<T>((complete) => {
        resolve = complete;
    });
    return { promise, resolve };
}

function connectWorkflow(workflow: ImageSessionWorkflow, loadVolume = vi.fn(async () => undefined)): void {
    workflow.connect({
        catalog: { activeVolumeId: '', loadVolume, clear: vi.fn() },
        audition: { invalidateSession: vi.fn(async () => undefined) },
        mutation: { setCapabilities: vi.fn() },
        clearExportSelection: vi.fn(),
    } as never);
}

describe('ImageSessionWorkflow open progress', () => {
    it.each(['resolve', 'reject'] as const)('ignores a stale integrity %s after a clean revision', async (outcome) => {
        const image = opened(7);
        image.format = 'ex5-disk';
        image.validation.warningCount = 1;
        let resolve!: (issues: []) => void;
        let reject!: (error: Error) => void;
        const pending = new Promise<[]>((yes, no) => {
            resolve = yes;
            reject = no;
        });
        const validationIssues = vi.fn(() => pending);
        const transport = {
            openImage: vi.fn(async () => image),
            refreshImage: vi.fn(async () => ({ ...opened(7), revision: 2 })),
            closeImage: vi.fn(async () => undefined),
            validationIssues,
        };
        const workflow = new ImageSessionWorkflow(transport as unknown as ImageTransport, {} as PickerController);
        connectWorkflow(workflow);
        const opening = workflow.open(location);
        await vi.waitFor(() => expect(validationIssues).toHaveBeenCalledOnce());
        await workflow.refresh();
        if (outcome === 'resolve') resolve([]);
        else reject(new Error('stale validation failure'));
        await opening;
        expect(workflow.integrityDialogOpen).toBe(false);
        expect(workflow.integrityLoading).toBe(false);
        expect(workflow.integrityError).toBe('');
        expect(workflow.integrityIssues).toEqual([]);
    });

    it('still opens the integrity dialog for an SFS allocation blocker', async () => {
        const image = opened(7);
        image.validation.errorCount = 1;
        image.validation.valid = false;
        const transport = {
            openImage: vi.fn(async () => image),
            closeImage: vi.fn(async () => undefined),
            validationIssues: vi.fn(async () => [
                {
                    code: 'SFS_ALLOCATION_CROSS_LINK',
                    severity: 'ERROR',
                    message: 'Cross-linked allocation',
                    samplerPath: '/',
                    objectId: null,
                },
            ]),
        };
        const workflow = new ImageSessionWorkflow(transport as unknown as ImageTransport, {} as PickerController);
        connectWorkflow(workflow);
        await workflow.open(location);
        expect(workflow.integrityDialogOpen).toBe(true);
    });

    it('shows EX warnings once, retaining them on refresh and reopening for a new issue', async () => {
        const image = opened(7);
        image.format = 'ex5-disk';
        image.validation.warningCount = 1;
        const issue = {
            code: 'EX5_CAPACITY_EXCEEDS_IMAGE',
            severity: 'WARNING' as const,
            message: 'One sector is absent',
            samplerPath: '/',
            objectId: null,
        };
        const validationIssues = vi.fn(async () => [issue]);
        const transport = {
            openImage: vi.fn(async () => image),
            refreshImage: vi.fn(async () => ({ ...image, revision: 2 })),
            closeImage: vi.fn(async () => undefined),
            validationIssues,
        };
        const workflow = new ImageSessionWorkflow(transport as unknown as ImageTransport, {} as PickerController);
        connectWorkflow(workflow);
        await workflow.open(location);
        expect(workflow.integrityDialogOpen).toBe(true);
        workflow.integrityDialogOpen = false;
        await workflow.refresh();
        expect(workflow.integrityDialogOpen).toBe(false);
        expect(workflow.integrityIssues).toEqual([issue]);
        validationIssues.mockResolvedValue([
            issue,
            { ...issue, code: 'EX5_FILE_DATA_UNAVAILABLE', samplerPath: 'TAIL.BIN', message: 'File data is absent' },
        ]);
        await workflow.refresh();
        expect(workflow.integrityDialogOpen).toBe(true);
        workflow.integrityDialogOpen = false;
        validationIssues.mockResolvedValue([issue]);
        await workflow.refresh();
        expect(workflow.integrityDialogOpen).toBe(false);
    });
    it('shows delayed progress and cancels an active server job', async () => {
        vi.useFakeTimers();
        try {
            const openImage = vi.fn(
                (_location: ImageLocation, options?: Parameters<ImageTransport['openImage']>[1]) =>
                    new Promise<OpenedImage>((_resolve, reject) => {
                        options?.onUpdate?.({
                            jobId: 1,
                            kind: 'images.open',
                            status: 'running',
                            progress: { phase: 0, completed: 2, total: 5, label: 'Resolving sampler objects' },
                        });
                        options?.signal?.addEventListener(
                            'abort',
                            () => reject(new DOMException('Image opening was cancelled', 'AbortError')),
                            { once: true },
                        );
                    }),
            );
            const workflow = new ImageSessionWorkflow(
                {
                    openImage,
                    closeImage: vi.fn(async () => undefined),
                } as unknown as ImageTransport,
                {} as PickerController,
            );
            connectWorkflow(workflow);

            const opening = workflow.open(location);
            await vi.advanceTimersByTimeAsync(749);
            expect(workflow.openProgressVisible).toBe(false);
            await vi.advanceTimersByTimeAsync(1);
            expect(workflow.openProgressVisible).toBe(true);
            expect(workflow.openProgressLabel).toBe('Resolving sampler objects');
            expect(workflow.openProgressCancellable).toBe(true);

            workflow.cancelOpen();
            await opening;

            expect(workflow.status).toBe('Image opening cancelled');
            expect(workflow.openProgressVisible).toBe(false);
            await workflow.dispose();
        } finally {
            vi.useRealTimers();
        }
    });

    it('shows non-cancellable workspace preparation after the server job completes', async () => {
        vi.useFakeTimers();
        try {
            const result = deferred<OpenedImage>();
            const openImage = vi.fn(
                (_location: ImageLocation, options?: Parameters<ImageTransport['openImage']>[1]) => {
                    options?.onUpdate?.({
                        jobId: 1,
                        kind: 'images.open',
                        status: 'completed',
                        progress: { phase: 0, completed: 5, total: 5, label: 'Image session ready' },
                    });
                    return result.promise;
                },
            );
            const workflow = new ImageSessionWorkflow(
                {
                    openImage,
                    closeImage: vi.fn(async () => undefined),
                } as unknown as ImageTransport,
                {} as PickerController,
            );
            connectWorkflow(workflow);

            const opening = workflow.open(location);
            await vi.advanceTimersByTimeAsync(750);

            expect(workflow.openProgressVisible).toBe(true);
            expect(workflow.openProgressLabel).toBe('Preparing workspace');
            expect(workflow.openProgressCancellable).toBe(false);

            result.resolve(opened(1));
            await opening;
            expect(workflow.openProgressVisible).toBe(false);
            await workflow.dispose();
        } finally {
            vi.useRealTimers();
        }
    });
});

describe('ImageSessionWorkflow lease maintenance', () => {
    it('reopens an expired image session at the selected volume', async () => {
        const openImage = vi.fn().mockResolvedValueOnce(opened(1)).mockResolvedValueOnce(opened(2));
        const loadVolume = vi.fn(async () => undefined);
        const transport = {
            openImage,
            keepImageAlive: vi.fn(async () => {
                throw new AxklibApiError('image_not_found', 'Image session does not exist', 404);
            }),
            closeImage: vi.fn(async () => undefined),
        } as unknown as ImageTransport;
        const workflow = new ImageSessionWorkflow(transport, {} as PickerController);
        workflow.connect({
            catalog: { loadVolume, clear: vi.fn() },
            audition: { invalidateSession: vi.fn(async () => undefined) },
            mutation: { setCapabilities: vi.fn() },
            clearExportSelection: vi.fn(),
        } as never);

        await workflow.open(location, { partitionIndex: 0, volumeName: 'newvolume' });
        await workflow.maintainLease();

        expect(openImage).toHaveBeenCalledTimes(2);
        expect(workflow.sessionId).toBe(2);
        expect(workflow.selectedSource).toMatchObject({ name: 'newvolume', partitionIndex: 0 });
        expect(loadVolume).toHaveBeenLastCalledWith('volume-2', 0);
        await workflow.dispose();
    });
});

describe('ImageSessionWorkflow volume selection', () => {
    it('uses one volume as an import target and only a shared partition for a multi-selection', async () => {
        const transport = { closeImage: vi.fn(async () => undefined) } as unknown as ImageTransport;
        const workflow = new ImageSessionWorkflow(transport, {} as PickerController);
        workflow.connect({
            catalog: { activeVolumeId: '', loadVolume: vi.fn(async () => undefined), clear: vi.fn() },
        } as never);
        const volumeA = {
            id: 'volume-a',
            name: 'A',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const volumeB = {
            id: 'volume-b',
            name: 'B',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const volumeC = {
            id: 'volume-c',
            name: 'C',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 1,
        };
        const partitionA = {
            id: 'partition-a',
            name: 'PARTITION 1',
            kind: 'partition' as const,
            childCount: 2,
            partitionIndex: 0,
            children: [volumeA, volumeB],
        };
        const partitionB = {
            id: 'partition-b',
            name: 'PARTITION 2',
            kind: 'partition' as const,
            childCount: 1,
            partitionIndex: 1,
            children: [volumeC],
        };
        workflow.sourceItems = [partitionA, partitionB];
        const visible = [volumeA, volumeB, volumeC];

        workflow.selectTreeSource(volumeA, 'replace', visible);
        expect(workflow.importDestinationSource()).toMatchObject({ id: 'volume-a', kind: 'volume' });

        workflow.selectTreeSource(volumeB, 'toggle', visible);
        expect(workflow.importDestinationSource()).toMatchObject({ id: 'partition-a', kind: 'partition' });

        workflow.selectTreeSource(volumeC, 'toggle', visible);
        expect(workflow.importDestinationSource()).toMatchObject({ id: 'none', kind: 'disk' });
        await workflow.dispose();
    });
});

describe('ImageSessionWorkflow companion folders', () => {
    const folder: ImageLocation = {
        kind: 'axk-object-directory',
        reference: { rootId: 'root', relativePath: 'set/disk1' },
        displayName: 'disk1',
    };
    const disk2: ImageLocation = { ...folder, reference: { rootId: 'root', relativePath: 'set/disk2' } };
    function diskSet(next: number | null): OpenedImage {
        return {
            ...opened(7),
            format: 'axk-object-directory',
            floppySet: {
                status: next === null ? 'COMPLETE' : 'INCOMPLETE',
                setLabel: '              ',
                nextRequiredIndex: next,
                members: [],
            },
        };
    }

    it.each(['local', 'remote'])(
        'opens the server folder picker directly for a %s connection',
        async (connectionMode) => {
            const onPicker = vi.fn();
            const picker = new PickerController(onPicker);
            const workflow = new ImageSessionWorkflow(
                {
                    storageMode: 'server',
                    connectionMode,
                    openImage: vi.fn(async () => diskSet(2)),
                } as unknown as ImageTransport,
                picker,
            );
            connectWorkflow(workflow);
            await workflow.open(folder);
            expect(workflow.companionRequest).toMatchObject({
                sourceKind: 'directory',
                nextRequiredIndex: 2,
                setLabel: 'disk1',
            });
            const adding = workflow.addCompanionDiskSource();
            expect(onPicker).toHaveBeenLastCalledWith(
                expect.objectContaining({
                    mode: 'directory',
                    parentDialog: 'companion-disks',
                    requireWritableDirectory: false,
                    initialDirectory: { rootId: 'root', relativePath: 'set' },
                }),
            );
            picker.finish({ ...disk2, kind: 'server-directory' });
            await adding;
            expect(workflow.companionRequest?.sources).toEqual([disk2]);
            workflow.cancelCompanionDisks();
            expect(workflow.companionRequest).toBeNull();
            expect(workflow.sessionId).toBe(7);
        },
    );

    it('waits for the final member before retrying and preserves the partial session on failure', async () => {
        const playObject = vi.fn();
        const attachCompanions = vi
            .fn()
            .mockResolvedValueOnce(diskSet(3))
            .mockRejectedValueOnce(new Error('Wrong disk set'))
            .mockResolvedValueOnce(diskSet(null));
        const workflow = new ImageSessionWorkflow(
            { openImage: vi.fn(async () => diskSet(2)), attachCompanions } as unknown as ImageTransport,
            {} as PickerController,
        );
        workflow.connect({
            catalog: { activeVolumeId: '', loadVolume: vi.fn(), clear: vi.fn() },
            audition: { invalidateSession: vi.fn(), playObject },
            mutation: { setCapabilities: vi.fn() },
            clearExportSelection: vi.fn(),
        } as never);
        await workflow.open(folder);
        workflow.requestCompanionDisks({ kind: 'audition', objectId: 'wave-1' });
        const selection = { kind: 'sources', sources: [disk2] } as const;
        await workflow.attachCompanionDisks({ ...selection, sources: [disk2] });
        expect(workflow.companionRequest?.nextRequiredIndex).toBe(3);
        expect(playObject).not.toHaveBeenCalled();
        await workflow.attachCompanionDisks({ ...selection, sources: [disk2] });
        expect(workflow.companionRequest?.error).toBe('Wrong disk set');
        expect(workflow.sessionId).toBe(7);
        await workflow.attachCompanionDisks({ ...selection, sources: [disk2] });
        expect(playObject).toHaveBeenCalledExactlyOnceWith('wave-1');
        expect(workflow.companionRequest).toBeNull();
    });
});
