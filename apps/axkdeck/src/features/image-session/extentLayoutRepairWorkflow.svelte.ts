import { saveRetainedMedia, selectLocalMediaDestination } from '../../lib/nativeMediaExports';
import type { DirectoryRef, ImageLocation } from '../../lib/storageLocations';
import type {
    ImageSessionExtentLayoutRepairDestination,
    ImageSessionExtentLayoutRepairResult,
    ImageTransport,
} from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

interface ExtentLayoutRepairWorkflowDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    setStatus: (status: string) => void;
    closeDialog: () => void;
}

export class ExtentLayoutRepairWorkflow {
    busy = $state(false);
    jobId = $state<number | null>(null);
    progressLabel = $state('');
    error = $state('');
    private generation = 0;
    private lastWorkspaceDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: ExtentLayoutRepairWorkflowDependencies) {}

    async repair(): Promise<void> {
        if (this.busy) {
            await this.cancel();
            return;
        }
        const sessionId = this.dependencies.sessionId();
        const location = this.dependencies.imageLocation();
        if (sessionId === null || location === null) return;
        const generation = ++this.generation;
        const suggestedName = repairFilename(location.displayName);
        if (this.dependencies.isDesktop) {
            const localDestination = await selectLocalMediaDestination(suggestedName);
            if (!localDestination || generation !== this.generation) return;
            await this.run(
                sessionId,
                { kind: 'DOWNLOAD', filename: localDestination.filename },
                localDestination.candidateId,
                generation,
            );
            return;
        }
        const extension = fileExtension(suggestedName);
        const selection = await this.dependencies.picker.chooseLocation(
            'save-file',
            'Save repaired image copy',
            [extension],
            suggestedName,
            {
                parentDialog: 'integrity-repair',
                initialDirectory: this.lastWorkspaceDirectory,
                ondirectorychange: (directory) => (this.lastWorkspaceDirectory = directory),
            },
        );
        if (selection?.kind !== 'server-file' || generation !== this.generation) return;
        await this.run(
            sessionId,
            { kind: 'WORKSPACE', output: selection.reference, overwrite: false },
            null,
            generation,
        );
    }

    async cancel(): Promise<void> {
        ++this.generation;
        const jobId = this.jobId;
        this.progressLabel = 'Cancelling repair';
        if (jobId !== null) await this.dependencies.jobs.cancel(jobId).catch(() => undefined);
        this.reset();
        this.dependencies.setStatus('Image repair cancelled');
    }

    dispose(): void {
        ++this.generation;
        if (this.jobId !== null) void this.dependencies.jobs.cancel(this.jobId).catch(() => undefined);
        this.reset();
    }

    private async run(
        sessionId: number,
        destination: ImageSessionExtentLayoutRepairDestination,
        localCandidateId: string | null,
        generation: number,
    ): Promise<void> {
        this.busy = true;
        this.jobId = null;
        this.progressLabel = 'Preparing repaired image copy';
        this.error = '';
        this.dependencies.setStatus('Repairing image into a separate copy');
        let retained: ImageSessionExtentLayoutRepairResult['download'] = null;
        try {
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startExtentLayoutRepair(sessionId, destination),
                (update) => {
                    if (generation === this.generation && update.progress?.label) {
                        this.progressLabel = update.progress.label;
                    }
                },
                async (job) => {
                    if (generation !== this.generation) {
                        await this.dependencies.jobs.cancel(job.jobId).catch(() => undefined);
                        throw new Error('Image repair was superseded');
                    }
                    this.jobId = job.jobId;
                },
            );
            if (generation !== this.generation) return;
            if (completed.status === 'cancelled') {
                this.reset();
                this.dependencies.setStatus('Image repair cancelled');
                return;
            }
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Image repair did not complete');
            const result = completed.result as ImageSessionExtentLayoutRepairResult;
            retained = result.download;
            if (localCandidateId !== null) {
                if (!retained) throw new Error('Image repair did not provide a retained download');
                await saveRetainedMedia(localCandidateId, retained.contentPath, retained.sizeBytes);
            }
            this.reset();
            this.dependencies.closeDialog();
            this.dependencies.setStatus(`Saved repaired image copy ${result.defaultFilename}`);
        } catch (caught) {
            if (generation !== this.generation) return;
            const message = userFacingMessage(caught);
            this.busy = false;
            this.jobId = null;
            this.progressLabel = '';
            this.error = message;
            this.dependencies.setStatus(message);
        } finally {
            if (retained) await this.dependencies.transport.deleteRetainedPackage(retained).catch(() => undefined);
        }
    }

    private reset(): void {
        this.busy = false;
        this.jobId = null;
        this.progressLabel = '';
        this.error = '';
    }
}

function repairFilename(displayName: string): string {
    const separator = displayName.lastIndexOf('.');
    if (separator <= 0) return `${displayName}_repaired.hds`;
    return `${displayName.slice(0, separator)}_repaired${displayName.slice(separator)}`;
}

function fileExtension(filename: string): string {
    const separator = filename.lastIndexOf('.');
    return separator < 0 ? 'hds' : filename.slice(separator + 1).toLowerCase();
}
