import { saveRetainedSfzExport, selectLocalSfzDestination } from '../../lib/nativeAudioExports';
import { saveRetainedPackage, selectLocalPackageDestination } from '../../lib/nativePackages';
import { packageExportFilename } from '../../lib/packageExport';
import type { DirectoryRef, ImageLocation } from '../../lib/storageLocations';
import type {
    ImageSessionAudioExportDestination,
    ImageSessionAudioExportInspection,
    ImageSessionAudioExportResult,
    ImageSessionPackageExportResult,
    ImageTransport,
} from '../../lib/transport';
import type { PackageExportSelection } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { exportSelectionLabel, imageSessionExportRoots } from './selection';

export interface PackageExportRequest {
    items: PackageExportSelection[];
    busy: boolean;
    progressLabel: string;
    error: string;
}

export interface AudioExportRequest {
    items: PackageExportSelection[];
    inspection: ImageSessionAudioExportInspection | null;
    format: 'SFZ' | 'WAV';
    loading: boolean;
    busy: boolean;
    jobId: number | null;
    progressLabel: string;
    error: string;
}

export type PackageExportDestination =
    | { kind: 'WORKSPACE'; output: { rootId: string; relativePath: string }; overwrite: boolean }
    | { kind: 'DOWNLOAD'; filename: string };

export type ExportCompanionRetry =
    | {
          kind: 'package-export';
          destination: PackageExportDestination;
          localDestination?: { candidateId: string };
      }
    | {
          kind: 'audio-export';
          destination: ImageSessionAudioExportDestination;
          format: 'SFZ' | 'WAV';
          localDestination?: { candidateId: string };
      };

interface ExportWorkflowDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    setStatus: (status: string) => void;
    requestCompanionDisks: (retry: ExportCompanionRetry) => void;
}

export class ExportWorkflow {
    packageRequest = $state<PackageExportRequest | null>(null);
    audioRequest = $state<AudioExportRequest | null>(null);
    private generation = 0;
    private audioGeneration = 0;
    private lastPackageDirectory = $state<DirectoryRef | null>(null);
    private lastAudioDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: ExportWorkflowDependencies) {}

    dispose(): void {
        ++this.generation;
        ++this.audioGeneration;
        this.audioRequest = null;
        this.packageRequest = null;
    }

    requestPackage(items: PackageExportSelection[]): void {
        ++this.generation;
        this.packageRequest = { items: [...items], busy: false, progressLabel: '', error: '' };
    }

    closePackage(): void {
        if (this.packageRequest?.busy) return;
        ++this.generation;
        this.packageRequest = null;
    }

    async runPackage(destination: PackageExportDestination, localDestination?: { candidateId: string }): Promise<void> {
        const request = this.packageRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) return;
        const exportLabel = exportSelectionLabel(request.items);
        this.packageRequest = { ...request, busy: true, progressLabel: 'Building package', error: '' };
        this.dependencies.setStatus(`Exporting ${exportLabel}`);
        let retained: ImageSessionPackageExportResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startImagePackageExport(
                        sessionId,
                        imageSessionExportRoots(request.items),
                        destination,
                    ),
                (update) => {
                    if (this.packageRequest && update.progress?.label) {
                        this.packageRequest = { ...this.packageRequest, progressLabel: update.progress.label };
                    }
                },
            );
            if (completed.status !== 'completed') {
                if (completed.errorCode === 'companion_disks_required') {
                    this.packageRequest = { ...request, busy: false, progressLabel: '', error: '' };
                    this.dependencies.requestCompanionDisks({
                        kind: 'package-export',
                        destination,
                        localDestination,
                    });
                    return;
                }
                throw new Error(completed.error ?? 'Package export did not complete');
            }
            const result = completed.result as ImageSessionPackageExportResult;
            retained = result.download;
            if (localDestination) {
                if (!retained) throw new Error('Package export did not provide a retained download');
                await saveRetainedPackage(localDestination.candidateId, retained.contentPath, retained.sizeBytes);
            }
            this.packageRequest = null;
            this.dependencies.setStatus(`Exported ${exportLabel}`);
        } catch (error) {
            const message = userFacingMessage(error);
            this.dependencies.setStatus(message);
            if (this.packageRequest) {
                this.packageRequest = { ...this.packageRequest, busy: false, progressLabel: '', error: message };
            }
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    async packageToWorkspace(): Promise<void> {
        const request = this.packageRequest;
        if (!request || request.busy) return;
        const generation = this.generation;
        const filename = packageExportFilename(request.items, this.dependencies.imageLocation());
        const selection = await this.dependencies.picker.chooseLocation(
            'save-file',
            'Export axklib package',
            [filename.slice(filename.lastIndexOf('.') + 1)],
            filename,
            {
                parentDialog: 'package-export',
                initialDirectory: this.lastPackageDirectory,
                ondirectorychange: (directory) => (this.lastPackageDirectory = directory),
            },
        );
        if (selection?.kind !== 'server-file' || generation !== this.generation || !this.packageRequest) return;
        await this.runPackage({ kind: 'WORKSPACE', output: selection.reference, overwrite: false });
    }

    async packageToComputer(): Promise<void> {
        const request = this.packageRequest;
        if (!request || request.busy || !this.dependencies.isDesktop) return;
        const generation = this.generation;
        try {
            const destination = await selectLocalPackageDestination(
                packageExportFilename(request.items, this.dependencies.imageLocation()),
            );
            if (!destination || generation !== this.generation || !this.packageRequest) return;
            await this.runPackage(
                { kind: 'DOWNLOAD', filename: destination.filename },
                { candidateId: destination.candidateId },
            );
        } catch (error) {
            if (!this.packageRequest) return;
            const message = userFacingMessage(error);
            this.packageRequest = { ...this.packageRequest, error: message };
            this.dependencies.setStatus(message);
        }
    }

    async requestAudio(items: PackageExportSelection[]): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null || items.length === 0) return;
        const generation = ++this.audioGeneration;
        this.audioRequest = {
            items: [...items],
            inspection: null,
            format: 'SFZ',
            loading: true,
            busy: false,
            jobId: null,
            progressLabel: '',
            error: '',
        };
        try {
            const inspection = await this.dependencies.transport.inspectImageAudioExport(
                sessionId,
                imageSessionExportRoots(items),
            );
            if (
                generation !== this.audioGeneration ||
                this.dependencies.sessionId() !== sessionId ||
                !this.audioRequest
            )
                return;
            this.audioRequest = {
                ...this.audioRequest,
                inspection,
                format: inspection.sfzEligible ? 'SFZ' : 'WAV',
                loading: false,
            };
        } catch (error) {
            if (generation !== this.audioGeneration || !this.audioRequest) return;
            const message = userFacingMessage(error);
            this.audioRequest = { ...this.audioRequest, loading: false, error: message };
            this.dependencies.setStatus(message);
        }
    }

    async runAudio(
        destination: ImageSessionAudioExportDestination,
        format: 'SFZ' | 'WAV',
        localDestination?: { candidateId: string },
    ): Promise<void> {
        const request = this.audioRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection || sessionId === null || request.busy) return;
        const generation = this.audioGeneration;
        const label = exportSelectionLabel(request.items);
        this.audioRequest = {
            ...request,
            format,
            busy: true,
            jobId: null,
            progressLabel: 'Preparing audio export',
            error: '',
        };
        this.dependencies.setStatus(`Exporting audio from ${label}`);
        let retained: ImageSessionAudioExportResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startImageAudioExport(
                        sessionId,
                        imageSessionExportRoots(request.items),
                        format,
                        destination,
                    ),
                (update) => {
                    if (generation === this.audioGeneration && this.audioRequest && update.progress?.label) {
                        this.audioRequest = { ...this.audioRequest, progressLabel: update.progress.label };
                    }
                },
                async (job) => {
                    if (generation !== this.audioGeneration || !this.audioRequest) {
                        await this.dependencies.jobs.cancel(job.jobId).catch(() => undefined);
                        throw new Error('Audio export was superseded');
                    }
                    this.audioRequest = { ...this.audioRequest, jobId: job.jobId };
                },
            );
            if (completed.status === 'cancelled') {
                if (generation === this.audioGeneration) this.audioRequest = null;
                this.dependencies.setStatus('Audio export cancelled');
                return;
            }
            if (completed.status !== 'completed') {
                if (completed.errorCode === 'companion_disks_required') {
                    this.audioRequest = {
                        ...request,
                        format,
                        busy: false,
                        jobId: null,
                        progressLabel: '',
                        error: '',
                    };
                    this.dependencies.requestCompanionDisks({
                        kind: 'audio-export',
                        destination,
                        format,
                        localDestination,
                    });
                    return;
                }
                throw new Error(completed.error ?? 'Audio export did not complete');
            }
            const result = completed.result as ImageSessionAudioExportResult;
            retained = result.download;
            if (localDestination) {
                if (!retained) throw new Error('Audio export did not provide a retained download');
                await saveRetainedSfzExport(localDestination.candidateId, retained.contentPath, retained.sizeBytes);
            }
            if (generation === this.audioGeneration) this.audioRequest = null;
            this.dependencies.setStatus(`Exported audio from ${label}`);
        } catch (error) {
            if (generation !== this.audioGeneration || !this.audioRequest) return;
            const message = userFacingMessage(error);
            this.audioRequest = {
                ...this.audioRequest,
                busy: false,
                jobId: null,
                progressLabel: '',
                error: message,
            };
            this.dependencies.setStatus(message);
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    async audioToWorkspace(): Promise<void> {
        const request = this.audioRequest;
        if (!request?.inspection || request.busy) return;
        const generation = this.audioGeneration;
        const selection = await this.dependencies.picker.chooseLocation(
            'save-directory',
            'Export SFZ',
            [],
            request.inspection.defaultDirectoryName,
            {
                parentDialog: 'audio-export',
                initialDirectory: this.lastAudioDirectory,
                ondirectorychange: (directory) => (this.lastAudioDirectory = directory),
                requireWritableDirectory: true,
            },
        );
        if (selection?.kind !== 'server-directory' || generation !== this.audioGeneration || !this.audioRequest) return;
        await this.runAudio({ kind: 'WORKSPACE', output: selection.reference }, this.audioRequest.format);
    }

    async audioToComputer(): Promise<void> {
        const request = this.audioRequest;
        if (!request?.inspection || request.busy || !this.dependencies.isDesktop) return;
        const generation = this.audioGeneration;
        try {
            const destination = await selectLocalSfzDestination(request.inspection.defaultDirectoryName);
            if (!destination || generation !== this.audioGeneration || !this.audioRequest) return;
            await this.runAudio(
                { kind: 'DOWNLOAD', directoryName: destination.directoryName },
                this.audioRequest.format,
                { candidateId: destination.candidateId },
            );
        } catch (error) {
            if (generation !== this.audioGeneration || !this.audioRequest) return;
            const message = userFacingMessage(error);
            this.audioRequest = { ...this.audioRequest, error: message };
            this.dependencies.setStatus(message);
        }
    }

    setAudioFormat(format: 'SFZ' | 'WAV'): void {
        if (this.audioRequest && !this.audioRequest.busy) {
            this.audioRequest = { ...this.audioRequest, format };
        }
    }

    cancelAudio(): void {
        const request = this.audioRequest;
        if (!request) return;
        if (request.busy) {
            if (request.jobId === null) {
                ++this.audioGeneration;
                this.audioRequest = null;
                this.dependencies.setStatus('Cancelling audio export');
                return;
            }
            this.audioRequest = { ...request, progressLabel: 'Cancelling audio export' };
            void this.dependencies.jobs.cancel(request.jobId);
            return;
        }
        ++this.audioGeneration;
        this.audioRequest = null;
    }
}
