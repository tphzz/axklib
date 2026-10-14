import { documentTarget } from './httpNativeDocuments';
import type {
    AudioImportItem,
    AudioImportOptions,
    VolumeImportDestination,
    SampleBankCreation,
    SampleBankAssignment,
    SequenceImportItem,
    SequenceSystemExclusivePolicy,
    JobState,
    AudioSourceInfo,
    MidiInspection,
    AudioImportCapabilities,
} from './transport';
import type { InputFileLocation } from './storageLocations';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import { serverInput } from './httpTransportWire';
import type { CapacityAdmission, CapacityImport, CapacityPolicy } from './importCapacity';
import { filesystemEditWire } from './httpFilesystemInputs';

const ALTERATION_MANIFEST_SCHEMA_VERSION = '1.0';

interface ImportAlterationRequest extends Record<string, unknown> {
    imageId: string;
    documentId: string;
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
    async inspectCapacity(
        sessionId: number,
        input: CapacityImport,
        policy: CapacityPolicy,
    ): Promise<CapacityAdmission> {
        const session = this.imageSessions.get(sessionId);
        const request =
            input.kind === 'AUDIO'
                ? audioImportRequest(
                      documentTarget(session),
                      session.revision,
                      input.target,
                      input.items,
                      input.options,
                  )
                : input.kind === 'SEQUENCE'
                  ? sequenceImportRequest(
                        documentTarget(session),
                        session.revision,
                        input.target,
                        input.items,
                        input.systemExclusivePolicy,
                    )
                  : {
                        imageId: session.remoteId,
                        expectedRevision: input.expectedRevision,
                        acknowledgeDeviceRelationships: true,
                        edits: input.edits.map(filesystemEditWire),
                    };
        const result = await this.client.invoke<{ capacity: CapacityAdmission }>(
            input.kind === 'FILES' ? 'images.filesystem.edit.inspect' : 'images.alter.inspect',
            { ...request, capacityPolicy: policy },
        );
        if (this.jobs.isJob(result)) throw new Error('Capacity inspection unexpectedly returned a job');
        return result.capacity;
    }
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

    startAudioImport(
        sessionId: number,
        target: VolumeImportDestination,
        items: AudioImportItem[],
        options: AudioImportOptions,
        policy?: CapacityPolicy,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(
            audioImportRequest(documentTarget(session), session.revision, target, items, options),
            policy,
        );
    }

    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(sampleBankCreationRequest(documentTarget(session), session.revision, creation));
    }

    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(sampleBankAssignmentRequest(documentTarget(session), session.revision, assignment));
    }

    startSequenceImport(
        sessionId: number,
        target: VolumeImportDestination,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
        policy?: CapacityPolicy,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        return this.start(
            sequenceImportRequest(documentTarget(session), session.revision, target, items, systemExclusivePolicy),
            policy,
        );
    }

    private async start(request: ImportAlterationRequest, policy?: CapacityPolicy): Promise<JobState> {
        return this.imageSessions.mutations.start({ ...request, ...(policy ? { capacityPolicy: policy } : {}) });
    }
}

function request(
    document: ReturnType<typeof documentTarget>,
    expectedRevision: number,
    operations: Record<string, unknown>[],
    inputBindings: ImportAlterationRequest['inputBindings'],
): ImportAlterationRequest {
    return {
        ...document,
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
    document: ReturnType<typeof documentTarget>,
    expectedRevision: number,
    target: VolumeImportDestination,
    items: AudioImportItem[],
    { grouping, sampleFormat }: AudioImportOptions,
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
                storage_format: sampleFormat.toLowerCase(),
                waveform_name: item.waveformNames[0],
                ...(item.waveformNames[1] ? { right_waveform_name: item.waveformNames[1] } : {}),
                parameters: {
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
                storage_format: sampleFormat.toLowerCase(),
                member_samples: items.map((item) => item.sampleName),
            },
        });
    }
    return request(document, expectedRevision, operations, inputBindings);
}

export function sampleBankCreationRequest(
    document: ReturnType<typeof documentTarget>,
    expectedRevision: number,
    creation: SampleBankCreation,
): ImportAlterationRequest {
    return request(
        document,
        expectedRevision,
        [
            {
                id: 'sample-bank',
                type: 'insert_sbac',
                partition_index: creation.partitionIndex,
                volume_name: creation.volumeName,
                sample_bank: {
                    name: creation.sampleBankName,
                    storage_format: creation.sampleFormat === 'A3000_188' ? 'a3000_188' : 'a4000_a5000_224',
                    member_samples: creation.sampleNames,
                },
            },
        ],
        [],
    );
}

export function sampleBankAssignmentRequest(
    document: ReturnType<typeof documentTarget>,
    expectedRevision: number,
    assignment: SampleBankAssignment,
): ImportAlterationRequest {
    return request(
        document,
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
    document: ReturnType<typeof documentTarget>,
    expectedRevision: number,
    target: VolumeImportDestination,
    items: SequenceImportItem[],
    systemExclusivePolicy: SequenceSystemExclusivePolicy,
): ImportAlterationRequest {
    const operations: Record<string, unknown>[] = [];
    if (target.kind === 'CREATE_VOLUME') {
        operations.push({
            id: 'volume-midi-import',
            type: 'insert_volume',
            partition_index: target.partitionIndex,
            volume: { name: target.volumeName, waveforms: [], samples: [] },
        });
    }
    operations.push(
        ...items.map((item, index) => ({
            id: `sequence-${index}`,
            type: 'insert_sequence',
            partition_index: target.partitionIndex,
            volume_name: target.volumeName,
            sequence: {
                name: item.sequenceName,
                midi_path: `sequence/import-${index}.mid`,
                system_exclusive_policy: systemExclusivePolicy,
            },
        })),
    );
    const inputBindings = items.map((item, index) => ({
        manifestPath: `sequence/import-${index}.mid`,
        input: serverInput(item.source),
    }));
    return request(document, expectedRevision, operations, inputBindings);
}
