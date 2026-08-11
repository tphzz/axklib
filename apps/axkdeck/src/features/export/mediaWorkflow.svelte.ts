import { saveRetainedMedia, selectLocalMediaDestination } from '../../lib/nativeMediaExports';
import type { DirectoryRef } from '../../lib/storageLocations';
import type {
    ImageSessionMediaConversionDestination,
    ImageSessionMediaConversionInspection,
    ImageSessionMediaConversionResult,
    ImageSessionMediaConversionSelection,
    ImageTransport,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export interface MediaExportRequest {
    item: DiskTreeItem;
    selection: ImageSessionMediaConversionSelection;
    inspection: ImageSessionMediaConversionInspection | null;
    loading: boolean;
    busy: boolean;
    jobId: number | null;
    progressLabel: string;
    error: string;
}

interface MediaExportWorkflowDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    setStatus: (status: string) => void;
}

export class MediaExportWorkflow {
    request = $state<MediaExportRequest | null>(null);
    private generation = 0;
    private lastWorkspaceDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: MediaExportWorkflowDependencies) {}

    dispose(): void {
        ++this.generation;
        this.request = null;
    }

    async open(item: DiskTreeItem): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        const selection = selectionFor(item);
        if (sessionId === null || !selection) return;
        const generation = ++this.generation;
        this.request = {
            item,
            selection,
            inspection: null,
            loading: true,
            busy: false,
            jobId: null,
            progressLabel: '',
            error: '',
        };
        try {
            const inspection = await this.dependencies.transport.inspectImageMediaConversion(sessionId, selection);
            if (generation !== this.generation || this.dependencies.sessionId() !== sessionId || !this.request) return;
            this.request = { ...this.request, inspection, loading: false };
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, loading: false, error: message };
            this.dependencies.setStatus(message);
        }
    }

    async toWorkspace(): Promise<void> {
        const request = this.request;
        if (!request?.inspection?.canExport || request.busy) return;
        const generation = this.generation;
        const extension = extensionFor(request.inspection);
        const selection = await this.dependencies.picker.chooseLocation(
            'save-file',
            pickerTitle(request.inspection),
            [extension],
            request.inspection.defaultFilename,
            {
                parentDialog: 'media-export',
                initialDirectory: this.lastWorkspaceDirectory,
                ondirectorychange: (directory) => (this.lastWorkspaceDirectory = directory),
            },
        );
        if (selection?.kind !== 'server-file' || generation !== this.generation || !this.request) return;
        await this.run({ kind: 'WORKSPACE', output: selection.reference, overwrite: false });
    }

    async toComputer(): Promise<void> {
        const request = this.request;
        if (!request?.inspection?.canExport || request.busy || !this.dependencies.isDesktop) return;
        const generation = this.generation;
        try {
            const destination = await selectLocalMediaDestination(request.inspection.defaultFilename);
            if (!destination || generation !== this.generation || !this.request) return;
            await this.run(
                { kind: 'DOWNLOAD', filename: destination.filename },
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
        if (request.busy) {
            if (request.jobId === null) {
                ++this.generation;
                this.request = null;
                this.dependencies.setStatus('Cancelling media export');
                return;
            }
            this.request = { ...request, progressLabel: 'Cancelling media export' };
            void this.dependencies.jobs.cancel(request.jobId);
            return;
        }
        ++this.generation;
        this.request = null;
    }

    private async run(
        destination: ImageSessionMediaConversionDestination,
        localDestination?: { candidateId: string },
    ): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection?.canExport || sessionId === null || request.busy) return;
        const generation = this.generation;
        const label = mediaLabel(request.inspection);
        this.request = {
            ...request,
            busy: true,
            jobId: null,
            progressLabel: `Preparing ${label}`,
            error: '',
        };
        this.dependencies.setStatus(`Exporting ${label}`);
        let retained: ImageSessionMediaConversionResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startImageMediaConversion(sessionId, request.selection, destination),
                (update) => {
                    if (generation === this.generation && this.request && update.progress?.label) {
                        this.request = { ...this.request, progressLabel: update.progress.label };
                    }
                },
                async (job) => {
                    if (generation !== this.generation || !this.request) {
                        await this.dependencies.jobs.cancel(job.jobId).catch(() => undefined);
                        throw new Error('Media export was superseded');
                    }
                    this.request = { ...this.request, jobId: job.jobId };
                },
            );
            if (completed.status === 'cancelled') {
                if (generation === this.generation) this.request = null;
                this.dependencies.setStatus('Media export cancelled');
                return;
            }
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Media export did not complete');
            const result = completed.result as ImageSessionMediaConversionResult;
            retained = result.download;
            if (localDestination) {
                if (!retained) throw new Error('Media export did not provide a retained download');
                await saveRetainedMedia(localDestination.candidateId, retained.contentPath, retained.sizeBytes);
            }
            if (generation === this.generation) this.request = null;
            this.dependencies.setStatus(`Exported ${label}`);
        } catch (error) {
            if (generation !== this.generation || !this.request) return;
            const message = userFacingMessage(error);
            this.request = { ...this.request, busy: false, jobId: null, progressLabel: '', error: message };
            this.dependencies.setStatus(message);
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }
}

function selectionFor(item: DiskTreeItem): ImageSessionMediaConversionSelection | null {
    if (item.partitionIndex === undefined) return null;
    if (item.kind === 'partition') return { format: 'ISO9660', partitionIndex: item.partitionIndex };
    if (item.kind === 'volume' && item.volumeDirectoryId !== undefined) {
        return {
            format: 'FAT12_FLOPPY',
            partitionIndex: item.partitionIndex,
            volumeDirectoryId: item.volumeDirectoryId,
        };
    }
    return null;
}

function extensionFor(inspection: ImageSessionMediaConversionInspection): 'iso' | 'ima' | 'zip' {
    switch (inspection.outputExtension) {
        case '.iso':
            return 'iso';
        case '.ima':
            return 'ima';
        case '.zip':
            return 'zip';
        default:
            throw new Error(`Unsupported media export extension: ${inspection.outputExtension}`);
    }
}

function pickerTitle(inspection: ImageSessionMediaConversionInspection): string {
    if (inspection.artifactKind === 'FLOPPY_DISK_SET') return 'Export floppy disk set';
    return inspection.format === 'ISO9660' ? 'Export CD-ROM image' : 'Export floppy image';
}

function mediaLabel(inspection: ImageSessionMediaConversionInspection): string {
    if (inspection.artifactKind === 'FLOPPY_DISK_SET') return 'floppy disk set';
    return inspection.format === 'ISO9660' ? 'CD-ROM image' : 'floppy image';
}
