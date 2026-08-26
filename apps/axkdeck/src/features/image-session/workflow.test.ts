import { describe, expect, it, vi } from 'vitest';
import { AxklibApiError } from '../../lib/httpErrors';
import type { ImageLocation } from '../../lib/storageLocations';
import type { ImageTransport, OpenedImage } from '../../lib/transport';
import type { PickerController } from '../dialogs/picker';
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
