import type {
    AudioImportItem,
    AudioImportTarget,
    SequenceImportItem,
    SequenceImportTarget,
    SequenceSystemExclusivePolicy,
} from './transport';
import { serverInput } from './httpTransportWire';

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
    target: AudioImportTarget,
    items: AudioImportItem[],
): ImportAlterationRequest {
    const operations: Record<string, unknown>[] = [];
    const inputBindings: ImportAlterationRequest['inputBindings'] = [];
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
    return request(imageId, expectedRevision, operations, inputBindings);
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
