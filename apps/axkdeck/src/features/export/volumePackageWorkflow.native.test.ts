import { beforeEach, describe, expect, it, vi } from 'vitest';
import { serverDirectoryLocation } from '../../lib/storageLocations';
import type {
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumePackageExportResult,
    ImageTransport,
    JobState,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { DirectComputerWorkflow } from '../file-operations/directComputerWorkflow';
import { JobController } from '../jobs/actions';
import { VolumePackageExportWorkflow } from './volumePackageWorkflow.svelte';

const native = vi.hoisted(() => ({
    select: vi.fn(),
    save: vi.fn(),
    cancel: vi.fn(),
}));

vi.mock('../../lib/nativeDirectoryExports', () => ({
    selectLocalDirectoryExportDestination: native.select,
    saveRetainedDirectoryExport: native.save,
    cancelRetainedDirectoryExport: native.cancel,
}));

interface SourceSnapshot {
    sessionId: number;
    documentId: string | null;
    revision: number;
}

const originalSource: SourceSnapshot = { sessionId: 12, documentId: 'document-one', revision: 4 };
const scope = {
    id: 'partition-one',
    name: 'A3000 ProSnd Lib',
    kind: 'partition',
    childCount: 15,
    partitionIndex: 0,
} as DiskTreeItem;
const secondScope = { ...scope, id: 'partition-two', name: 'Second CD-ROM group' };
const destination = { candidateId: 'candidate-one', directoryName: 'A3000 packages' };
const download = {
    archiveId: 'archive-one',
    contentPath: '/api/v1/download-archives/archive-one/content',
    expiresInSeconds: 300,
    filename: 'A3000 packages.tar',
    sizeBytes: 8192,
};

function inspection(
    overrides: Partial<ImageSessionVolumePackageExportInspection> = {},
): ImageSessionVolumePackageExportInspection {
    return {
        imageId: 'image-one',
        revision: 4,
        sourceMediaKind: 'ISO9660',
        scopeId: scope.id,
        scopeName: scope.name,
        defaultDirectoryName: 'A3000 packages',
        volumeCount: 15,
        exportableCount: 15,
        emptyCount: 0,
        volumes: [],
        ...overrides,
    };
}

function completed(overrides: Partial<ImageSessionVolumePackageExportResult> = {}): JobState {
    return {
        jobId: 19,
        kind: 'images.volume_package_export',
        status: 'completed',
        result: {
            imageId: 'image-one',
            revision: 4,
            scopeId: scope.id,
            volumeCount: 15,
            exportedCount: 15,
            skippedCount: 0,
            failedCount: 0,
            reportPath: 'volume-packages.axklib.json',
            volumes: [],
            destination: 'DOWNLOAD',
            output: null,
            download,
            ...overrides,
        } satisfies ImageSessionVolumePackageExportResult,
    };
}

function deferred<T>() {
    let resolve!: (value: T) => void;
    const promise = new Promise<T>((complete) => {
        resolve = complete;
    });
    return { promise, resolve };
}

function harness() {
    let source: SourceSnapshot | null = { ...originalSource };
    const inspect = vi.fn().mockImplementation(async (_sessionId: number, scopeId: string) =>
        inspection({
            scopeId,
            scopeName: scopeId === scope.id ? scope.name : secondScope.name,
        }),
    );
    const start = vi.fn().mockResolvedValue({
        jobId: 19,
        kind: 'images.volume_package_export',
        status: 'queued',
    });
    const wait = vi.fn().mockResolvedValue(completed());
    const cancelJob = vi.fn().mockResolvedValue(undefined);
    const remove = vi.fn().mockResolvedValue(undefined);
    const chooseLocation = vi.fn();
    const setStatus = vi.fn();
    const transport = {
        inspectImageVolumePackageExport: inspect,
        startImageVolumePackageExport: start,
        waitForJob: wait,
        cancelJob,
        deleteRetainedPackage: remove,
    } as unknown as ImageTransport;
    const dependencies = {
        transport,
        jobs: new JobController(transport),
        picker: { chooseLocation } as never,
        isDesktop: true,
        session: () => (source ? { ...source } : null),
        setStatus,
    };
    const workflow = new VolumePackageExportWorkflow(dependencies);
    return {
        workflow,
        inspect,
        start,
        wait,
        cancelJob,
        remove,
        chooseLocation,
        setStatus,
        setSource: (next: SourceSnapshot | null) => {
            source = next;
        },
    };
}

beforeEach(() => {
    vi.resetAllMocks();
    native.select.mockResolvedValue(destination);
    native.save.mockResolvedValue(undefined);
    native.cancel.mockResolvedValue(undefined);
});

describe('native volume-package export ownership', () => {
    it('preserves native-only recovery after a failed export without choosing server storage', async () => {
        const h = harness();
        h.wait.mockResolvedValueOnce({
            jobId: 19,
            kind: 'images.volume_package_export',
            status: 'failed',
            error: 'download archive cache is unavailable',
        });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(h.workflow.request?.destinationFlow).toBe('DIRECT_COMPUTER');
        expect(h.workflow.request?.error).toContain('Download archive cache is unavailable');
        expect(h.workflow.request?.busy).toBe(false);
        expect(h.chooseLocation).not.toHaveBeenCalled();
        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(native.save).not.toHaveBeenCalled();
    });

    it('exports exactly the inspected target and publishes the selected native folder once', async () => {
        const h = harness();
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(native.select).toHaveBeenCalledOnce();
        expect(native.select).toHaveBeenCalledWith('A3000 packages', 'Packages');
        expect(h.start).toHaveBeenCalledExactlyOnceWith(
            12,
            {
                imageId: 'image-one',
                documentId: 'document-one',
                scopeId: 'partition-one',
                expectedRevision: 4,
            },
            { kind: 'DOWNLOAD', directoryName: 'A3000 packages' },
        );
        expect(native.save).toHaveBeenCalledExactlyOnceWith(
            destination.candidateId,
            download.contentPath,
            download.sizeBytes,
        );
        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
        expect(h.workflow.request).toBeNull();
        expect(h.setStatus).toHaveBeenLastCalledWith('Exported 15 volume packages');
        expect(h.chooseLocation).not.toHaveBeenCalled();
    });

    it('closes a cancelled native chooser without submitting an export', async () => {
        const h = harness();
        native.select.mockResolvedValueOnce(null);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(h.workflow.request).toBeNull();
        expect(h.start).not.toHaveBeenCalled();
        expect(native.save).not.toHaveBeenCalled();
        expect(native.cancel).not.toHaveBeenCalled();
        expect(h.chooseLocation).not.toHaveBeenCalled();
    });

    it('retains native-only error recovery if opening the OS chooser fails', async () => {
        const h = harness();
        native.select.mockRejectedValueOnce(new Error('native chooser unavailable'));
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(h.workflow.request?.destinationFlow).toBe('DIRECT_COMPUTER');
        expect(h.workflow.request?.error).toContain('Native chooser unavailable');
        expect(h.start).not.toHaveBeenCalled();
        expect(h.chooseLocation).not.toHaveBeenCalled();
    });

    it('releases the candidate when the backend confirms cancellation', async () => {
        const h = harness();
        h.wait.mockResolvedValueOnce({ jobId: 19, kind: 'images.volume_package_export', status: 'cancelled' });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(native.save).not.toHaveBeenCalled();
        expect(h.workflow.request).toBeNull();
        expect(h.remove).not.toHaveBeenCalled();
    });

    it('cleans the retained archive and candidate after native publication fails', async () => {
        const h = harness();
        native.save.mockRejectedValueOnce(new Error('export destination is unavailable'));
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(h.workflow.request?.destinationFlow).toBe('DIRECT_COMPUTER');
        expect(h.workflow.request?.error).toContain('Export destination is unavailable');
        expect(h.chooseLocation).not.toHaveBeenCalled();
    });

    it('does not offer a retry after the native folder has already been published', async () => {
        const h = harness();
        native.save.mockImplementationOnce(async () => {
            h.setSource({ ...originalSource, revision: 5 });
        });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(h.workflow.request).toBeNull();
        expect(h.setStatus).toHaveBeenLastCalledWith('Exported 15 volume packages');
        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
    });

    it('closes intentional cancellation during native saving instead of offering Retry', async () => {
        const h = harness();
        let rejectSave!: (error: Error) => void;
        native.save.mockReturnValueOnce(
            new Promise<void>((_, reject) => {
                rejectSave = reject;
            }),
        );
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const exporting = h.workflow.toComputer(true);
        await vi.waitFor(() => expect(native.save).toHaveBeenCalledOnce());

        h.workflow.cancel();
        rejectSave(new Error('export cancelled'));
        await exporting;

        expect(h.workflow.request).toBeNull();
        expect(h.setStatus).toHaveBeenLastCalledWith('Volume package export cancelled');
        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
    });

    it('does not open a second native chooser while selection is pending', async () => {
        const h = harness();
        const selection = deferred<typeof destination>();
        native.select.mockReturnValue(selection.promise);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const first = h.workflow.toComputer(true);
        const second = h.workflow.toComputer(true);
        const pickerCount = native.select.mock.calls.length;
        selection.resolve(destination);
        await Promise.all([first, second]);

        expect(pickerCount).toBe(1);
        expect(h.start).toHaveBeenCalledOnce();
        expect(native.save).toHaveBeenCalledOnce();
    });

    it('does not submit another job while the first export is running', async () => {
        const h = harness();
        const completion = deferred<JobState>();
        h.wait.mockReturnValueOnce(completion.promise);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const exporting = h.workflow.toComputer(true);
        await vi.waitFor(() => expect(h.wait).toHaveBeenCalledOnce());

        await h.workflow.toComputer(true);
        expect(native.select).toHaveBeenCalledOnce();
        expect(h.start).toHaveBeenCalledOnce();
        completion.resolve(completed());
        await exporting;
    });

    it.each(['documentId', 'revision'] as const)(
        'rejects a %s change during initial inspection before opening any native chooser',
        async (field) => {
            const h = harness();
            const inspected = deferred<ImageSessionVolumePackageExportInspection>();
            h.inspect.mockReturnValueOnce(inspected.promise);
            const opening = h.workflow.open(scope, 'DIRECT_COMPUTER');
            h.setSource({ ...originalSource, [field]: field === 'revision' ? 5 : 'document-two' });
            inspected.resolve(inspection());
            await opening;

            await h.workflow.toComputer(true);

            expect(native.select).not.toHaveBeenCalled();
            expect(h.start).not.toHaveBeenCalled();
            expect(h.workflow.request?.loading ?? false).toBe(false);
        },
    );

    it('invalidates an inspection if its source session is replaced', async () => {
        const h = harness();
        const inspected = deferred<ImageSessionVolumePackageExportInspection>();
        h.inspect.mockReturnValueOnce(inspected.promise);
        const opening = h.workflow.open(scope, 'DIRECT_COMPUTER');
        h.setSource({ ...originalSource, sessionId: 13 });
        inspected.resolve(inspection());
        await opening;

        expect(h.workflow.request).toBeNull();
        expect(native.select).not.toHaveBeenCalled();
        expect(h.start).not.toHaveBeenCalled();
    });

    it.each(['sessionId', 'documentId', 'revision'] as const)(
        'releases native selection without submitting after a %s change in the OS chooser',
        async (field) => {
            const h = harness();
            const selection = deferred<typeof destination>();
            native.select.mockReturnValueOnce(selection.promise);
            await h.workflow.open(scope, 'DIRECT_COMPUTER');
            const exporting = h.workflow.toComputer(true);
            h.setSource({
                ...originalSource,
                [field]: field === 'sessionId' ? 13 : field === 'revision' ? 5 : 'document-two',
            });
            selection.resolve(destination);
            await exporting;

            expect(h.start).not.toHaveBeenCalled();
            expect(native.save).not.toHaveBeenCalled();
            expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
            if (field === 'sessionId') expect(h.workflow.request).toBeNull();
        },
    );

    it('releases a stale native selection without changing a newer request', async () => {
        const h = harness();
        const selection = deferred<typeof destination>();
        native.select.mockReturnValueOnce(selection.promise);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const oldExport = h.workflow.toComputer(true);
        await h.workflow.open(secondScope, 'DIRECT_COMPUTER');
        selection.resolve(destination);
        await oldExport;

        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(h.start).not.toHaveBeenCalled();
        expect(h.workflow.request?.scope.id).toBe(secondScope.id);
        expect(h.workflow.request?.error).toBe('');
        expect(h.workflow.request?.busy).toBe(false);
    });

    it('never publishes an obsolete completed job into a new request', async () => {
        const h = harness();
        const completion = deferred<JobState>();
        h.wait.mockReturnValueOnce(completion.promise);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const oldExport = h.workflow.toComputer(true);
        await vi.waitFor(() => expect(h.wait).toHaveBeenCalledOnce());
        await h.workflow.open(secondScope, 'DIRECT_COMPUTER');
        completion.resolve(completed());
        await oldExport;

        expect(native.save).not.toHaveBeenCalled();
        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
        expect(h.workflow.request?.scope.id).toBe(secondScope.id);
        expect(h.workflow.request?.error).toBe('');
        expect(h.setStatus).not.toHaveBeenCalledWith('Exported 15 volume packages');
    });

    it('re-inspects an explicit Retry before opening the chooser and binds the refreshed revision', async () => {
        const h = harness();
        const nextInspection = deferred<ImageSessionVolumePackageExportInspection>();
        const nextDestination = { candidateId: 'candidate-two', directoryName: 'Retry packages' };
        h.wait.mockResolvedValueOnce({
            jobId: 19,
            kind: 'images.volume_package_export',
            status: 'failed',
            error: 'download archive cache is unavailable',
        });
        native.select.mockResolvedValueOnce(destination).mockResolvedValueOnce(nextDestination);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        await h.workflow.toComputer(true);
        h.setSource({ ...originalSource, revision: 5 });
        h.inspect.mockReturnValueOnce(nextInspection.promise);
        h.wait.mockResolvedValueOnce(completed({ revision: 5 }));

        const retry = h.workflow.retryToComputer();
        expect(h.workflow.request?.destinationFlow).toBe('DIRECT_COMPUTER');
        expect(h.workflow.request?.loading).toBe(true);
        expect(native.select).toHaveBeenCalledOnce();
        expect(h.start).toHaveBeenCalledOnce();
        nextInspection.resolve(inspection({ revision: 5 }));
        await retry;

        expect(h.inspect).toHaveBeenCalledTimes(2);
        expect(native.select).toHaveBeenCalledTimes(2);
        expect(h.start).toHaveBeenLastCalledWith(
            12,
            {
                imageId: 'image-one',
                documentId: 'document-one',
                scopeId: scope.id,
                expectedRevision: 5,
            },
            { kind: 'DOWNLOAD', directoryName: 'Retry packages' },
        );
        expect(native.save).toHaveBeenCalledExactlyOnceWith(
            nextDestination.candidateId,
            download.contentPath,
            download.sizeBytes,
        );
        expect(h.chooseLocation).not.toHaveBeenCalled();
    });

    it('rejects another revision change during Retry inspection without reopening the chooser', async () => {
        const h = harness();
        h.wait.mockResolvedValueOnce({
            jobId: 19,
            kind: 'images.volume_package_export',
            status: 'failed',
            error: 'download archive cache is unavailable',
        });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        await h.workflow.toComputer(true);
        const nextInspection = deferred<ImageSessionVolumePackageExportInspection>();
        h.inspect.mockReturnValueOnce(nextInspection.promise);
        const retry = h.workflow.retryToComputer();
        h.setSource({ ...originalSource, revision: 5 });
        nextInspection.resolve(inspection());
        await retry;

        expect(native.select).toHaveBeenCalledOnce();
        expect(h.start).toHaveBeenCalledOnce();
        expect(h.workflow.request?.loading ?? false).toBe(false);
    });

    it('does not inspect or retry an export after the original session has changed', async () => {
        const h = harness();
        h.wait.mockResolvedValueOnce({
            jobId: 19,
            kind: 'images.volume_package_export',
            status: 'failed',
            error: 'download archive cache is unavailable',
        });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        await h.workflow.toComputer(true);
        h.setSource({ ...originalSource, sessionId: 13 });

        await h.workflow.retryToComputer();

        expect(h.inspect).toHaveBeenCalledOnce();
        expect(native.select).toHaveBeenCalledOnce();
        expect(h.start).toHaveBeenCalledOnce();
        expect(h.workflow.request).toBeNull();
    });

    it('never opens a native chooser or submits a job for an all-empty scope', async () => {
        const h = harness();
        h.inspect.mockResolvedValueOnce(inspection({ exportableCount: 0, emptyCount: 15 }));
        await h.workflow.open(scope, 'DIRECT_COMPUTER');

        await h.workflow.toComputer(true);

        expect(native.select).not.toHaveBeenCalled();
        expect(h.start).not.toHaveBeenCalled();
        expect(h.workflow.request?.destinationFlow).toBe('DIRECT_COMPUTER');
    });

    it('preserves the server-directory choice when the request was not direct', async () => {
        const h = harness();
        const output = { rootId: 'workspace', relativePath: 'exports/A3000 packages' };
        h.chooseLocation.mockResolvedValueOnce(serverDirectoryLocation(output));
        h.wait.mockResolvedValueOnce(completed({ destination: 'WORKSPACE', output, download: null }));
        await h.workflow.open(scope);
        expect(h.workflow.request?.destinationFlow).toBe('CHOOSER');

        await h.workflow.toWorkspace();

        expect(h.chooseLocation).toHaveBeenCalledOnce();
        expect(h.start).toHaveBeenCalledExactlyOnceWith(
            12,
            {
                imageId: 'image-one',
                documentId: 'document-one',
                scopeId: scope.id,
                expectedRevision: 4,
            },
            { kind: 'WORKSPACE', output },
        );
        expect(native.select).not.toHaveBeenCalled();
        expect(native.save).not.toHaveBeenCalled();
    });

    it('does not let an old initiating inspection export a newer chooser request', async () => {
        const h = harness();
        const oldInspection = deferred<ImageSessionVolumePackageExportInspection>();
        h.inspect.mockReturnValueOnce(oldInspection.promise);
        const coordinator = new DirectComputerWorkflow(true, 'local');
        const oldAction = coordinator.exportVolumePackages(h.workflow, scope);
        await h.workflow.open(secondScope, 'CHOOSER');
        oldInspection.resolve(inspection());
        await oldAction;

        expect(native.select).not.toHaveBeenCalled();
        expect(h.start).not.toHaveBeenCalled();
        expect(h.workflow.request?.scope.id).toBe(secondScope.id);
        expect(h.workflow.request?.destinationFlow).toBe('CHOOSER');
    });

    it('does not let an old Retry inspection export a newer chooser request', async () => {
        const h = harness();
        h.wait.mockResolvedValueOnce({
            jobId: 19,
            kind: 'images.volume_package_export',
            status: 'failed',
            error: 'download archive cache is unavailable',
        });
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        await h.workflow.toComputer(true);
        native.select.mockClear();
        h.start.mockClear();
        const oldInspection = deferred<ImageSessionVolumePackageExportInspection>();
        h.inspect.mockReturnValueOnce(oldInspection.promise);
        const oldRetry = h.workflow.retryToComputer();
        await h.workflow.open(secondScope, 'CHOOSER');
        oldInspection.resolve(inspection());
        await oldRetry;

        expect(native.select).not.toHaveBeenCalled();
        expect(h.start).not.toHaveBeenCalled();
        expect(h.workflow.request?.scope.id).toBe(secondScope.id);
        expect(h.workflow.request?.destinationFlow).toBe('CHOOSER');
    });

    it('ignores another initiating action while its native chooser is pending', async () => {
        const h = harness();
        const selection = deferred<typeof destination>();
        native.select.mockReturnValueOnce(selection.promise).mockResolvedValueOnce(null);
        const coordinator = new DirectComputerWorkflow(true, 'local');
        const exporting = coordinator.exportVolumePackages(h.workflow, scope);
        await vi.waitFor(() => expect(native.select).toHaveBeenCalledOnce());

        await coordinator.exportVolumePackages(h.workflow, secondScope);
        const selectedScope = h.workflow.request?.scope.id;
        const pickerCount = native.select.mock.calls.length;
        selection.resolve(destination);
        await exporting;

        expect(pickerCount).toBe(1);
        expect(selectedScope).toBe(scope.id);
        expect(h.start).toHaveBeenCalledOnce();
        expect(h.start.mock.calls[0]?.[1]).toMatchObject({ scopeId: scope.id });
    });

    it('ignores another initiating action while its native export job is active', async () => {
        const h = harness();
        const completion = deferred<JobState>();
        native.select.mockResolvedValueOnce(destination).mockResolvedValueOnce(null);
        h.wait.mockReturnValueOnce(completion.promise);
        const coordinator = new DirectComputerWorkflow(true, 'local');
        const exporting = coordinator.exportVolumePackages(h.workflow, scope);
        await vi.waitFor(() => expect(h.wait).toHaveBeenCalledOnce());

        await coordinator.exportVolumePackages(h.workflow, secondScope);
        const selectedScope = h.workflow.request?.scope.id;
        const pickerCount = native.select.mock.calls.length;
        completion.resolve(completed());
        await exporting;

        expect(pickerCount).toBe(1);
        expect(selectedScope).toBe(scope.id);
        expect(h.start).toHaveBeenCalledOnce();
        expect(native.save).toHaveBeenCalledOnce();
    });

    it('drains a superseded start result and deletes its retained archive without publishing it', async () => {
        const h = harness();
        const started = deferred<JobState>();
        h.start.mockReturnValueOnce(started.promise);
        await h.workflow.open(scope, 'DIRECT_COMPUTER');
        const oldExport = h.workflow.toComputer(true);
        await vi.waitFor(() => expect(h.start).toHaveBeenCalledOnce());
        h.workflow.dispose();
        await h.workflow.open(secondScope, 'CHOOSER');
        started.resolve({ jobId: 19, kind: 'images.volume_package_export', status: 'queued' });
        await oldExport;

        expect(h.cancelJob).toHaveBeenCalledWith(19);
        expect(h.wait).toHaveBeenCalledOnce();
        expect(h.remove).toHaveBeenCalledExactlyOnceWith(download);
        expect(native.save).not.toHaveBeenCalled();
        expect(native.cancel).toHaveBeenCalledWith(destination.candidateId);
        expect(h.workflow.request?.scope.id).toBe(secondScope.id);
        expect(h.workflow.request?.destinationFlow).toBe('CHOOSER');
        expect(h.workflow.request?.error).toBe('');
        expect(h.setStatus).not.toHaveBeenCalledWith('Exported 15 volume packages');
    });
});
