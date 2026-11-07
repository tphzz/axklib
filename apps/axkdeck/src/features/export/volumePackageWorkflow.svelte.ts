import {
    cancelRetainedDirectoryExport,
    saveRetainedDirectoryExport,
    selectLocalDirectoryExportDestination,
} from '../../lib/nativeDirectoryExports';
import type { DirectoryRef } from '../../lib/storageLocations';
import type {
    ImageSessionVolumePackageExportDestination,
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumePackageExportResult,
    ImageSessionVolumePackageExportTarget,
    ImageTransport,
    OpenedImage,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export type VolumePackageExportDestinationFlow = 'CHOOSER' | 'DIRECT_COMPUTER';
type ExportSource = Pick<OpenedImage, 'sessionId' | 'documentId' | 'revision'>;

export interface VolumePackageExportRequest {
    scope: DiskTreeItem;
    source: ExportSource;
    target: ImageSessionVolumePackageExportTarget | null;
    destinationFlow: VolumePackageExportDestinationFlow;
    inspection: ImageSessionVolumePackageExportInspection | null;
    loading: boolean;
    picking: boolean;
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
    session: () => ExportSource | null;
    setStatus: (status: string) => void;
}

export class VolumePackageExportWorkflow {
    request = $state<VolumePackageExportRequest | null>(null);
    private generation = 0;
    private cancelledGeneration = -1;
    private nativeActionPending = false;
    private lastDirectory = $state<DirectoryRef | null>(null);
    private nativeCandidate: { id: string; generation: number } | null = null;

    constructor(private readonly dependencies: Dependencies) {}

    dispose(): void {
        ++this.generation;
        if (this.nativeCandidate) {
            void cancelRetainedDirectoryExport(this.nativeCandidate.id).catch(() => undefined);
            this.nativeCandidate = null;
        }
        if (this.request?.busy && this.request.jobId !== null) {
            void this.dependencies.jobs.cancel(this.request.jobId).catch(() => undefined);
        }
        this.request = null;
    }

    private fail(error: unknown, generation: number): void {
        if (generation !== this.generation || !this.request) return;
        const message = userFacingMessage(error);
        this.request = {
            ...this.request,
            loading: false,
            picking: false,
            busy: false,
            jobId: null,
            progressLabel: '',
            error: message,
        };
        this.dependencies.setStatus(message);
    }

    private current(request: VolumePackageExportRequest, generation: number): boolean {
        if (generation !== this.generation || !this.request) return false;
        const source = this.dependencies.session();
        if (
            !source ||
            source.sessionId !== request.source.sessionId ||
            source.documentId !== request.source.documentId
        ) {
            this.dispose();
            this.dependencies.setStatus('The export source changed. Start the export again.');
            return false;
        }
        if (source.revision !== request.source.revision) {
            this.request = { ...this.request, target: null };
            this.fail(new Error('The export source changed. Retry to inspect it again.'), generation);
            return false;
        }
        return true;
    }

    private async inspect(
        scope: DiskTreeItem,
        destinationFlow: VolumePackageExportDestinationFlow,
        previous: ImageSessionVolumePackageExportInspection | null,
    ): Promise<number | null> {
        this.dispose();
        const source = this.dependencies.session();
        if (!source?.documentId || scope.kind !== 'partition') return null;
        const generation = this.generation;
        const request: VolumePackageExportRequest = {
            scope,
            source: { ...source },
            target: null,
            destinationFlow,
            inspection: previous,
            loading: true,
            picking: false,
            busy: false,
            jobId: null,
            progressLabel: '',
            error: '',
        };
        this.request = request;
        try {
            const inspection = await this.dependencies.transport.inspectImageVolumePackageExport(
                source.sessionId,
                scope.id,
            );
            if (!this.current(request, generation)) return null;
            if (inspection.revision !== source.revision || inspection.scopeId !== scope.id) {
                throw new Error('The export inspection no longer matches the selected source.');
            }
            this.request = {
                ...this.request!,
                inspection,
                loading: false,
                target: Object.freeze({
                    imageId: inspection.imageId,
                    documentId: source.documentId,
                    scopeId: inspection.scopeId,
                    expectedRevision: inspection.revision,
                }),
            };
            return generation;
        } catch (error) {
            if (this.current(request, generation)) this.fail(error, generation);
            return null;
        }
    }

    async open(scope: DiskTreeItem, destinationFlow: VolumePackageExportDestinationFlow = 'CHOOSER'): Promise<void> {
        await this.inspect(scope, destinationFlow, null);
    }

    async openToComputer(scope: DiskTreeItem): Promise<void> {
        if (this.nativeActionPending || this.request?.busy) return;
        const generation = await this.inspect(scope, 'DIRECT_COMPUTER', null);
        if (generation === this.generation && this.request?.target) await this.toComputer(true);
    }

    async retryToComputer(): Promise<void> {
        const request = this.request;
        if (!request || request.loading || request.picking || request.busy || this.nativeActionPending) return;
        const source = this.dependencies.session();
        if (
            !source ||
            source.sessionId !== request.source.sessionId ||
            source.documentId !== request.source.documentId
        ) {
            this.dispose();
            this.dependencies.setStatus('The export source changed. Start the export again.');
            return;
        }
        const generation = await this.inspect(request.scope, request.destinationFlow, request.inspection);
        if (generation === this.generation && this.request?.target) await this.toComputer(true);
    }

    async run(
        destination: ImageSessionVolumePackageExportDestination,
        localDestination?: { candidateId: string },
    ): Promise<void> {
        const request = this.request;
        const generation = this.generation;
        if (
            !request?.inspection ||
            !request.target ||
            request.busy ||
            request.loading ||
            request.inspection.exportableCount === 0 ||
            !this.current(request, generation)
        )
            return;
        const target = request.target;
        this.request = {
            ...request,
            busy: true,
            jobId: null,
            progressLabel: 'Building volume packages',
            error: '',
        };
        this.dependencies.setStatus('Exporting volume packages from ' + request.inspection.scopeName);
        let retained: ImageSessionVolumePackageExportResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startImageVolumePackageExport(
                        request.source.sessionId,
                        target,
                        destination,
                    ),
                (update) => {
                    if (generation === this.generation && this.request && update.progress?.label) {
                        this.request = { ...this.request, progressLabel: update.progress.label };
                    }
                },
                async (job) => {
                    if (!this.current(request, generation)) {
                        await this.dependencies.jobs.cancel(job.jobId).catch(() => undefined);
                        return;
                    }
                    this.request = { ...this.request!, jobId: job.jobId };
                },
            );
            const result =
                completed.status === 'completed' ? (completed.result as ImageSessionVolumePackageExportResult) : null;
            retained = result?.download ?? null;
            if (!this.current(request, generation)) return;
            if (completed.status === 'cancelled' || this.cancelledGeneration === generation) {
                this.request = null;
                this.dependencies.setStatus('Volume package export cancelled');
                return;
            }
            if (!result) throw new Error(completed.error ?? 'Volume package export did not complete');
            if (localDestination) {
                if (!retained) throw new Error('Volume package export did not provide a retained download');
                await saveRetainedDirectoryExport(
                    localDestination.candidateId,
                    retained.contentPath,
                    retained.sizeBytes,
                );
                // Publication succeeded: a later source change must not turn it into a retryable failure.
                if (generation !== this.generation || !this.request) return;
            }
            this.request = null;
            const exceptions = result.failedCount + result.skippedCount;
            const packageLabel =
                result.exportedCount + ' volume ' + (result.exportedCount === 1 ? 'package' : 'packages');
            this.dependencies.setStatus(
                exceptions === 0
                    ? 'Exported ' + packageLabel
                    : 'Exported ' + packageLabel + '; ' + exceptions + ' recorded in the report',
            );
        } catch (error) {
            if (this.cancelledGeneration === generation && generation === this.generation) {
                this.request = null;
                this.dependencies.setStatus('Volume package export cancelled');
            } else if (this.current(request, generation)) this.fail(error, generation);
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    async toWorkspace(): Promise<void> {
        const request = this.request;
        if (
            !request?.target ||
            !request.inspection ||
            request.loading ||
            request.picking ||
            request.busy ||
            request.inspection.exportableCount === 0
        )
            return;
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
        if (selection?.kind !== 'server-directory' || !this.current(request, generation)) return;
        await this.run({ kind: 'WORKSPACE', output: selection.reference });
    }

    async toComputer(closeOnCancel = false): Promise<void> {
        const request = this.request;
        const generation = this.generation;
        if (
            !request?.target ||
            !request.inspection ||
            request.loading ||
            request.picking ||
            request.busy ||
            this.nativeActionPending ||
            request.inspection.exportableCount === 0 ||
            !this.dependencies.isDesktop ||
            !this.current(request, generation)
        )
            return;
        this.nativeActionPending = true;
        this.request = { ...request, picking: true, error: '' };
        let destination: Awaited<ReturnType<typeof selectLocalDirectoryExportDestination>> = null;
        try {
            destination = await selectLocalDirectoryExportDestination(
                request.inspection.defaultDirectoryName,
                'Packages',
            );
            if (!destination) {
                if (closeOnCancel && generation === this.generation) this.cancel();
                return;
            }
            if (!this.current(request, generation)) return;
            this.request = { ...this.request!, picking: false };
            this.nativeCandidate = { id: destination.candidateId, generation };
            await this.run(
                { kind: 'DOWNLOAD', directoryName: destination.directoryName },
                { candidateId: destination.candidateId },
            );
        } catch (error) {
            if (this.current(request, generation)) this.fail(error, generation);
        } finally {
            if (destination) await cancelRetainedDirectoryExport(destination.candidateId).catch(() => undefined);
            if (this.nativeCandidate?.generation === generation) this.nativeCandidate = null;
            this.nativeActionPending = false;
            if (generation === this.generation && this.request?.picking) {
                this.request = { ...this.request, picking: false };
            }
        }
    }

    cancel(): void {
        const request = this.request;
        if (!request) return;
        if (request.busy && request.jobId !== null) {
            this.cancelledGeneration = this.generation;
            if (this.nativeCandidate)
                void cancelRetainedDirectoryExport(this.nativeCandidate.id).catch(() => undefined);
            void this.dependencies.jobs.cancel(request.jobId).catch(() => undefined);
            return;
        }
        this.dispose();
    }
}
