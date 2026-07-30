import type { AudioImportItem, AudioImportTarget, SequenceImportItem, SequenceImportTarget } from './transport';
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
                key_low: 0,
                key_high: 127,
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
): ImportAlterationRequest {
    const operations = items.map((item, index) => ({
        id: `sequence-${index}`,
        type: 'insert_sequence',
        partition_index: target.partitionIndex,
        volume_name: target.volumeName,
        sequence: {
            name: item.sequenceName,
            midi_path: `sequence/import-${index}.mid`,
        },
    }));
    const inputBindings = items.map((item, index) => ({
        manifestPath: `sequence/import-${index}.mid`,
        input: serverInput(item.source),
    }));
    return request(imageId, expectedRevision, operations, inputBindings);
}
