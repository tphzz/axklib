import { saveRetainedDirectoryExport, selectLocalDirectoryExportDestination } from '../../lib/nativeDirectoryExports';
import type { DirectoryRef } from '../../lib/storageLocations';
import type {
    ImageSessionVolumePackageExportDestination,
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumePackageExportResult,
    ImageTransport,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export interface VolumePackageExportRequest {
    scope: DiskTreeItem;
    inspection: ImageSessionVolumePackageExportInspection | null;
    loading: boolean;
    busy: boolean;
    jobId: number | null;
    progressLabel: string;
    error: string;
}

interface Dependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    setStatus: (status: string) => void;
}

export class VolumePackageExportWorkflow {
    request = $state<VolumePackageExportRequest | null>(null);
    private generation = 0;
    private lastDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: Dependencies) {}

    dispose(): void {
        ++this.generation;
        this.request = null;
    }

    async open(scope: DiskTreeItem): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null || scope.kind !== 'partition') return;
        const generation = ++this.generation;
        this.request = {
            scope,
            inspection: null,
            loading: true,
            busy: false,
            jobId: null,
            progressLabel: '',
            error: '',
        };
        try {
            const inspection = await this.dependencies.transport.inspectImageVolumePackageExport(sessionId, scope.id);
            if (generation !== this.generation || this.dependencies.sessionId() !== sessionId || !this.request) return;
            this.request = { ...this.request, inspection, loading: false };
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, loading: false, error: message };
            this.dependencies.setStatus(message);
        }
    }

    async run(
        destination: ImageSessionVolumePackageExportDestination,
        localDestination?: { candidateId: string },
    ): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection || request.busy || request.inspection.exportableCount === 0 || sessionId === null) {
            return;
        }
        const generation = this.generation;
        this.request = {
            ...request,
            busy: true,
            jobId: null,
            progressLabel: 'Building volume packages',
            error: '',
        };
        this.dependencies.setStatus(`Exporting volume packages from ${request.inspection.scopeName}`);
        let retained: ImageSessionVolumePackageExportResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startImageVolumePackageExport(sessionId, request.scope.id, destination),
                (update) => {
                    if (generation === this.generation && this.request && update.progress?.label) {
                        this.request = { ...this.request, progressLabel: update.progress.label };
                    }
                },
                async (job) => {
                    if (generation !== this.generation || !this.request) {
                        await this.dependencies.jobs.cancel(job.jobId).catch(() => undefined);
                        throw new Error('Volume package export was superseded');
                    }
                    this.request = { ...this.request, jobId: job.jobId };
                },
            );
            if (completed.status === 'cancelled') {
                if (generation === this.generation) this.request = null;
                this.dependencies.setStatus('Volume package export cancelled');
                return;
            }
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Volume package export did not complete');
            }
            const result = completed.result as ImageSessionVolumePackageExportResult;
            retained = result.download;
            if (localDestination) {
                if (!retained) throw new Error('Volume package export did not provide a retained download');
                await saveRetainedDirectoryExport(
                    localDestination.candidateId,
                    retained.contentPath,
                    retained.sizeBytes,
                );
            }
            if (generation === this.generation) this.request = null;
            const exceptions = result.failedCount + result.skippedCount;
            const packageLabel = `${result.exportedCount} volume ${result.exportedCount === 1 ? 'package' : 'packages'}`;
            this.dependencies.setStatus(
                exceptions === 0
                    ? `Exported ${packageLabel}`
                    : `Exported ${packageLabel}; ${exceptions} recorded in the report`,
            );
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, busy: false, jobId: null, progressLabel: '', error: message };
            this.dependencies.setStatus(message);
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    async toWorkspace(): Promise<void> {
        const request = this.request;
        if (!request?.inspection || request.busy || request.inspection.exportableCount === 0) return;
        const generation = this.generation;
        const selection = await this.dependencies.picker.chooseLocation(
            'save-directory',
            'Export volume packages',
            [],
            request.inspection.defaultDirectoryName,
            {
                parentDialog: 'volume-package-export',
                initialDirectory: this.lastDirectory,
                ondirectorychange: (directory) => (this.lastDirectory = directory),
                requireWritableDirectory: true,
            },
        );
        if (selection?.kind !== 'server-directory' || generation !== this.generation || !this.request) return;
        await this.run({ kind: 'WORKSPACE', output: selection.reference });
    }

    async toComputer(closeOnCancel = false): Promise<void> {
        const request = this.request;
        if (
            !request?.inspection ||
            request.busy ||
            request.inspection.exportableCount === 0 ||
            !this.dependencies.isDesktop
        ) {
            return;
        }
        const generation = this.generation;
        try {
            const destination = await selectLocalDirectoryExportDestination(
                request.inspection.defaultDirectoryName,
                'Packages',
            );
            if (!destination) {
                if (closeOnCancel) this.cancel();
                return;
            }
            if (generation !== this.generation || !this.request) return;
            await this.run(
                { kind: 'DOWNLOAD', directoryName: destination.directoryName },
                { candidateId: destination.candidateId },
            );
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, error: message };
            this.dependencies.setStatus(message);
        }
    }

    cancel(): void {
        const request = this.request;
        if (!request) return;
        if (request.busy && request.jobId !== null) {
            void this.dependencies.jobs.cancel(request.jobId);
            return;
        }
        ++this.generation;
        this.request = null;
    }
}
