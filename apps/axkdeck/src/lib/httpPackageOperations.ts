import type { components } from './generated/axklibApiV1';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import { randomIdempotencyKey, serverFile, serverInput } from './httpTransportWire';
import type { FileLocation, InputFileLocation } from './storageLocations';
import type {
    ImageSessionAudioExportDestination,
    ImageSessionAudioExportInspection,
    ImageSessionExportRoot,
    ImageSessionPackageExportDestination,
    ImageSessionVolumePackageExportDestination,
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumeFloppyExportDestination,
    ImageSessionVolumeFloppyExportInspection,
    ImageSessionPackageImportPlan,
    ImageSessionPackageProgramSlotAssignment,
    ImageSessionPackageRename,
    PackageOpaqueSequenceDecision,
    JobState,
    ImageSessionSequenceExportDestination,
    ImageSessionMediaConversionDestination,
    ImageSessionMediaConversionInspection,
    ImageSessionMediaConversionSelection,
    PackageImportDestination,
    PackageImportPlan,
    PackageInspection,
} from './transport';

export class HttpPackageOperations {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        private readonly imageSessions: HttpImageSessions,
    ) {}

    async inspect(source: InputFileLocation, verify: boolean): Promise<PackageInspection> {
        const result = await this.client.invoke<PackageInspection>(verify ? 'package.verify' : 'package.inspect', {
            package: serverInput(source),
        });
        if (this.jobs.isJob(result)) throw new Error('package inspection unexpectedly returned a job');
        return result;
    }

    async planImport(
        target: FileLocation,
        output: FileLocation,
        packages: InputFileLocation[],
        destinations: PackageImportDestination[],
        overwrite: boolean,
    ): Promise<PackageImportPlan> {
        const result = await this.client.invoke<PackageImportPlan>('package.plan_import', {
            target: serverFile(target).reference,
            output: serverFile(output).reference,
            packages: packages.map((source) => serverInput(source)),
            destinations,
            renames: [],
            programSlotAssignments: [],
            overwrite,
        });
        if (this.jobs.isJob(result)) throw new Error('package import planning unexpectedly returned a job');
        return result;
    }

    async startImport(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'package.import',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('package.import did not return a job');
        return this.jobs.map(job);
    }

    async planImageImport(
        sessionId: number,
        source: InputFileLocation,
        partitionIndex: number,
        volumeName: string,
        renames: ImageSessionPackageRename[] = [],
        programSlotAssignments: ImageSessionPackageProgramSlotAssignment[] = [],
        replacePlanToken?: string,
        opaqueSequenceDecisions: PackageOpaqueSequenceDecision[] = [],
    ): Promise<ImageSessionPackageImportPlan> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<ImageSessionPackageImportPlan>('images.package_import.plan', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            package: serverInput(source),
            partitionIndex,
            volumeName,
            renames,
            programSlotAssignments,
            opaqueSequenceDecisions,
            ...(replacePlanToken ? { replacePlanToken } : {}),
        });
        if (this.jobs.isJob(result)) throw new Error('image package import planning unexpectedly returned a job');
        return result;
    }

    async releaseImageImportPlan(planToken: string): Promise<void> {
        const result = await this.client.invoke<{ released: true }>('images.package_import.release', { planToken });
        if (this.jobs.isJob(result)) throw new Error('image package plan release unexpectedly returned a job');
    }

    async startImageImport(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'images.package_import',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.package_import did not return a job');
        return this.jobs.map(job);
    }

    async startImageExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        destination: ImageSessionPackageExportDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.package_export',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                roots,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.package_export did not return a job');
        return this.jobs.map(job);
    }

    async inspectVolumePackageExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumePackageExportInspection> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<ImageSessionVolumePackageExportInspection>(
            'images.volume_package_export.inspect',
            { imageId: session.remoteId, expectedRevision: session.revision, scopeId },
        );
        if (this.jobs.isJob(result)) {
            throw new Error('images.volume_package_export.inspect unexpectedly returned a job');
        }
        return result;
    }

    async startVolumePackageExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumePackageExportDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.volume_package_export',
            { imageId: session.remoteId, expectedRevision: session.revision, scopeId, destination },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.volume_package_export did not return a job');
        return this.jobs.map(job);
    }

    async inspectVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumeFloppyExportInspection> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<ImageSessionVolumeFloppyExportInspection>(
            'images.volume_floppy_export.inspect',
            { imageId: session.remoteId, expectedRevision: session.revision, scopeId },
        );
        if (this.jobs.isJob(result)) {
            throw new Error('images.volume_floppy_export.inspect unexpectedly returned a job');
        }
        return result;
    }

    async startVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumeFloppyExportDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.volume_floppy_export',
            { imageId: session.remoteId, expectedRevision: session.revision, scopeId, destination },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.volume_floppy_export did not return a job');
        return this.jobs.map(job);
    }

    async inspectAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
    ): Promise<ImageSessionAudioExportInspection> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<ImageSessionAudioExportInspection>('images.audio_export.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            roots,
        });
        if (this.jobs.isJob(result)) throw new Error('images.audio_export.inspect unexpectedly returned a job');
        return result;
    }

    async startAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        format: 'SFZ' | 'WAV',
        destination: ImageSessionAudioExportDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.audio_export',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                roots,
                format,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.audio_export did not return a job');
        return this.jobs.map(job);
    }

    async startSequenceExport(
        sessionId: number,
        objectIds: string[],
        destination: ImageSessionSequenceExportDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.sequence_export',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                objectIds,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.sequence_export did not return a job');
        return this.jobs.map(job);
    }

    async inspectMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
    ): Promise<ImageSessionMediaConversionInspection> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<ImageSessionMediaConversionInspection>(
            'images.media_conversion.inspect',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                ...selection,
            },
        );
        if (this.jobs.isJob(result)) throw new Error('images.media_conversion.inspect unexpectedly returned a job');
        return result;
    }

    async startMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
        destination: ImageSessionMediaConversionDestination,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.media_conversion',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                ...selection,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.media_conversion did not return a job');
        return this.jobs.map(job);
    }

    deleteRetained(download: components['schemas']['RetainedDownload']): Promise<void> {
        return this.client.deleteRetainedDownload(download.contentPath);
    }
}
