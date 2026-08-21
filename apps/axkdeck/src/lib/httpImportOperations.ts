import type {
    AudioImportItem,
    AudioImportGrouping,
    AudioImportDestination,
    AudioImportTarget,
    SampleBankCreation,
    SampleBankAssignment,
    SequenceImportItem,
    SequenceImportTarget,
    SequenceSystemExclusivePolicy,
    JobState,
    AudioSourceInfo,
    MidiInspection,
    Tx16wImportInspection,
    Tx16wImportMode,
    AudioImportCapabilities,
} from './transport';
import type { InputFileLocation } from './storageLocations';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import { randomIdempotencyKey, serverInput } from './httpTransportWire';

const ALTERATION_MANIFEST_SCHEMA_VERSION = '1.0';

interface ImportAlterationRequest extends Record<string, unknown> {
    imageId: string;
    expectedRevision: number;
    manifest: {
        inline: {
            schema_version: string;
            operations: Record<string, unknown>[];
        };
    };
    inputBindings: { manifestPath: string; input: ReturnType<typeof serverInput> }[];
}

export class HttpImportOperations {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        private readonly imageSessions: HttpImageSessions,
    ) {}

    async capabilities(): Promise<AudioImportCapabilities> {
        const serverCapabilities = await this.client.serverCapabilities();
        const capabilities = serverCapabilities.audioImport;
        if (!capabilities) throw new Error('The connected server does not publish audio import capabilities');
        return { ...capabilities, maximumUploads: serverCapabilities.limits.maximumUploads };
    }

    async inspectAudio(source: InputFileLocation, targetSampleRate?: number): Promise<AudioSourceInfo> {
        const result = await this.client.invoke<AudioSourceInfo>('audio.inspect', {
            source: serverInput(source),
            ...(targetSampleRate === undefined ? {} : { targetSampleRate }),
        });
        if (this.jobs.isJob(result)) throw new Error('audio.inspect unexpectedly returned a job');
        return result;
    }

    async inspectMidi(source: InputFileLocation): Promise<MidiInspection> {
        const result = await this.client.invoke<MidiInspection>('midi.inspect', { source: serverInput(source) });
        if (this.jobs.isJob(result)) throw new Error('midi.inspect unexpectedly returned a job');
        return result;
    }

    async inspectTx16wDiskSet(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<Tx16wImportInspection> {
        const session = this.imageSessions.get(sessionId);
        const result = await this.client.invoke<Tx16wImportInspection>('images.tx16w.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            sources: sources.map(serverInput),
            target,
            importMode,
        });
        if (this.jobs.isJob(result)) throw new Error('images.tx16w.inspect unexpectedly returned a job');
        return result;
    }

    startAudioImport(
        sessionId: number,
        target: AudioImportDestination,
        items: AudioImportItem[],
        grouping: AudioImportGrouping,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(audioImportRequest(session.remoteId, session.revision, target, items, grouping));
    }

    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(sampleBankCreationRequest(session.remoteId, session.revision, creation));
    }

    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(sampleBankAssignmentRequest(session.remoteId, session.revision, assignment));
    }

    startSequenceImport(
        sessionId: number,
        target: SequenceImportTarget,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(
            sequenceImportRequest(session.remoteId, session.revision, target, items, systemExclusivePolicy),
        );
    }

    startTx16wDiskSetImport(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(tx16wDiskSetImportRequest(session.remoteId, session.revision, sources, target, importMode));
    }

    private async start(request: ImportAlterationRequest): Promise<JobState> {
        const job = await this.client.invoke<never>('images.alter', request, {
            idempotencyKey: randomIdempotencyKey(),
        });
        if (!this.jobs.isJob(job)) throw new Error('images.alter did not return a job');
        return this.jobs.map(job);
    }
}

export function tx16wDiskSetImportRequest(
    imageId: string,
    expectedRevision: number,
    sources: InputFileLocation[],
    target: AudioImportTarget,
    importMode: Tx16wImportMode,
): ImportAlterationRequest {
    const logicalPaths = sources.map((_, index) => `tx16w/disk-${String(index + 1).padStart(2, '0')}.ima`);
    return request(
        imageId,
        expectedRevision,
        [
            {
                id: 'tx16w-import',
                type: 'import_tx16w_disk_set',
                partition_index: target.partitionIndex,
                volume_name: target.volumeName,
                disk_paths: logicalPaths,
                import_mode: importMode === 'HIERARCHY' ? 'hierarchy' : 'wave_data_only',
            },
        ],
        sources.map((source, index) => ({ manifestPath: logicalPaths[index], input: serverInput(source) })),
    );
}

function request(
    imageId: string,
    expectedRevision: number,
    operations: Record<string, unknown>[],
    inputBindings: ImportAlterationRequest['inputBindings'],
): ImportAlterationRequest {
    return {
        imageId,
        expectedRevision,
        manifest: {
            inline: {
                schema_version: ALTERATION_MANIFEST_SCHEMA_VERSION,
                operations,
            },
        },
        inputBindings,
    };
}

export function audioImportRequest(
    imageId: string,
    expectedRevision: number,
    target: AudioImportDestination,
    items: AudioImportItem[],
    grouping: AudioImportGrouping,
): ImportAlterationRequest {
    const operations: Record<string, unknown>[] = [];
    const inputBindings: ImportAlterationRequest['inputBindings'] = [];
    if (target.kind === 'CREATE_VOLUME') {
        operations.push({
            id: 'volume-audio-import',
            type: 'insert_volume',
            partition_index: target.partitionIndex,
            volume: { name: target.volumeName, waveforms: [], samples: [] },
        });
    }
    items.forEach((item, index) => {
        const logicalPath = `audio/import-${index}`;
        operations.push({
            id: `wave-${index}`,
            type: 'insert_waveform',
            partition_index: target.partitionIndex,
            volume_name: target.volumeName,
            audio: {
                path: logicalPath,
                waveform_names: item.waveformNames,
                root_key: item.rootKey,
                fine_tune_cents: item.fineTuneCents,
                loop_mode: item.loopMode,
                loop_start_frame: item.loopStartFrame,
                loop_length_frames: item.loopLengthFrames,
                target_sample_rate: item.targetSampleRate,
            },
        });
        operations.push({
            id: `sample-${index}`,
            type: 'insert_sbnk',
            partition_index: target.partitionIndex,
            volume_name: target.volumeName,
            sample: {
                name: item.sampleName,
                waveform_name: item.waveformNames[0],
                ...(item.waveformNames[1] ? { right_waveform_name: item.waveformNames[1] } : {}),
                root_key: item.rootKey,
                fine_tune_cents: item.fineTuneCents,
                key_low: item.keyLow,
                key_high: item.keyHigh,
                velocity_low: item.velocityLow,
                velocity_high: item.velocityHigh,
                loop_mode: item.loopMode,
                loop_start_frame: item.loopStartFrame,
                loop_length_frames: item.loopLengthFrames,
                level: 100,
            },
        });
        inputBindings.push({ manifestPath: logicalPath, input: serverInput(item.source) });
    });
    if (grouping.kind === 'SAMPLE_BANK') {
        operations.push({
            id: 'sample-bank',
            type: 'insert_sbac',
            partition_index: target.partitionIndex,
            volume_name: target.volumeName,
            sample_bank: {
                name: grouping.sampleBankName,
                member_samples: items.map((item) => item.sampleName),
            },
        });
    }
    return request(imageId, expectedRevision, operations, inputBindings);
}

export function sampleBankCreationRequest(
    imageId: string,
    expectedRevision: number,
    creation: SampleBankCreation,
): ImportAlterationRequest {
    return request(
        imageId,
        expectedRevision,
        [
            {
                id: 'sample-bank',
                type: 'insert_sbac',
                partition_index: creation.partitionIndex,
                volume_name: creation.volumeName,
                sample_bank: {
                    name: creation.sampleBankName,
                    member_samples: creation.sampleNames,
                },
            },
        ],
        [],
    );
}

export function sampleBankAssignmentRequest(
    imageId: string,
    expectedRevision: number,
    assignment: SampleBankAssignment,
): ImportAlterationRequest {
    return request(
        imageId,
        expectedRevision,
        [
            {
                id: 'sample-bank-assignment',
                type: 'assign_sbac_members',
                partition_index: assignment.partitionIndex,
                volume_name: assignment.volumeName,
                sample_bank_name: assignment.sampleBankName,
                sample_names: assignment.sampleNames,
            },
        ],
        [],
    );
}

export function sequenceImportRequest(
    imageId: string,
    expectedRevision: number,
    target: SequenceImportTarget,
    items: SequenceImportItem[],
    systemExclusivePolicy: SequenceSystemExclusivePolicy,
): ImportAlterationRequest {
    const operations = items.map((item, index) => ({
        id: `sequence-${index}`,
        type: 'insert_sequence',
        partition_index: target.partitionIndex,
        volume_name: target.volumeName,
        sequence: {
            name: item.sequenceName,
            midi_path: `sequence/import-${index}.mid`,
            system_exclusive_policy: systemExclusivePolicy,
        },
    }));
    const inputBindings = items.map((item, index) => ({
        manifestPath: `sequence/import-${index}.mid`,
        input: serverInput(item.source),
    }));
    return request(imageId, expectedRevision, operations, inputBindings);
}
