import { describe, expect, it } from 'vitest';

import { clientUploadLocation, serverFileLocation } from './storageLocations';
import {
    audioImportRequest,
    sampleBankAssignmentRequest,
    sampleBankCreationRequest,
    sequenceImportRequest,
    tx16wDiskSetImportRequest,
} from './httpImportOperations';

describe('audioImportRequest', () => {
    it('creates a new volume and imports its audio in one alteration manifest', () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'audio/Tone.wav' });

        const result = audioImportRequest(
            'image-1',
            8,
            { kind: 'CREATE_VOLUME', partitionIndex: 1, volumeName: 'Imported' },
            [
                {
                    source,
                    sampleName: 'Tone',
                    waveformNames: ['Tone Wave'],
                    rootKey: 60,
                    fineTuneCents: 0,
                    keyLow: 0,
                    keyHigh: 127,
                    velocityLow: 0,
                    velocityHigh: 127,
                    loopMode: 4,
                    loopStartFrame: 0,
                    loopLengthFrames: 0,
                    targetSampleRate: 44_100,
                },
            ],
            { kind: 'SAMPLES' },
        );

        expect(result.manifest.inline.operations.map((operation) => operation.type)).toEqual([
            'insert_volume',
            'insert_waveform',
            'insert_sbnk',
        ]);
        expect(result.manifest.inline.operations[0]).toEqual({
            id: 'volume-audio-import',
            type: 'insert_volume',
            partition_index: 1,
            volume: { name: 'Imported', waveforms: [], samples: [] },
        });
        expect(result.inputBindings).toEqual([
            {
                manifestPath: 'audio/import-0',
                input: { fileRef: { rootId: 'workspace', relativePath: 'audio/Tone.wav' } },
            },
        ]);
    });
});

describe('sequenceImportRequest', () => {
    it('creates a new volume and imports its Sequences in one alteration manifest', () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'midi/Pattern.mid' });

        const result = sequenceImportRequest(
            'image-1',
            8,
            { kind: 'CREATE_VOLUME', partitionIndex: 1, volumeName: 'Imported' },
            [{ source, sequenceName: 'Pattern' }],
            'exclude',
        );

        expect(result.manifest.inline.operations).toEqual([
            {
                id: 'volume-midi-import',
                type: 'insert_volume',
                partition_index: 1,
                volume: { name: 'Imported', waveforms: [], samples: [] },
            },
            {
                id: 'sequence-0',
                type: 'insert_sequence',
                partition_index: 1,
                volume_name: 'Imported',
                sequence: {
                    name: 'Pattern',
                    midi_path: 'sequence/import-0.mid',
                    system_exclusive_policy: 'exclude',
                },
            },
        ]);
        expect(result.inputBindings).toEqual([
            {
                manifestPath: 'sequence/import-0.mid',
                input: { fileRef: { rootId: 'workspace', relativePath: 'midi/Pattern.mid' } },
            },
        ]);
    });
});

describe('sampleBankCreationRequest', () => {
    it('creates one ordered Sample Bank insertion without input bindings', () => {
        expect(
            sampleBankCreationRequest('image-1', 4, {
                partitionIndex: 2,
                volumeName: 'Keys',
                sampleBankName: 'Layered Keys',
                sampleNames: ['Piano 2', 'Piano 10'],
            }),
        ).toEqual({
            imageId: 'image-1',
            expectedRevision: 4,
            manifest: {
                inline: {
                    schema_version: '1.0',
                    operations: [
                        {
                            id: 'sample-bank',
                            type: 'insert_sbac',
                            partition_index: 2,
                            volume_name: 'Keys',
                            sample_bank: {
                                name: 'Layered Keys',
                                member_samples: ['Piano 2', 'Piano 10'],
                            },
                        },
                    ],
                },
            },
            inputBindings: [],
        });
    });
});

describe('tx16wDiskSetImportRequest', () => {
    it('binds every explicit disk member to the canonical disk-set operation', () => {
        const upload = clientUploadLocation({ uploadId: 'disk-a' }, 'DISK_IMAGE', 'set-a.img');
        const workspace = serverFileLocation({ rootId: 'workspace', relativePath: 'tx16w/set-b.ima' });

        expect(
            tx16wDiskSetImportRequest(
                'image-1',
                7,
                [upload, workspace],
                { partitionIndex: 1, volumeName: 'TX16W' },
                'WAVE_DATA_ONLY',
            ),
        ).toEqual({
            imageId: 'image-1',
            expectedRevision: 7,
            manifest: {
                inline: {
                    schema_version: '1.0',
                    operations: [
                        {
                            id: 'tx16w-import',
                            type: 'import_tx16w_disk_set',
                            partition_index: 1,
                            volume_name: 'TX16W',
                            disk_paths: ['tx16w/disk-01.ima', 'tx16w/disk-02.ima'],
                            import_mode: 'wave_data_only',
                        },
                    ],
                },
            },
            inputBindings: [
                { manifestPath: 'tx16w/disk-01.ima', input: { uploadRef: { uploadId: 'disk-a' } } },
                {
                    manifestPath: 'tx16w/disk-02.ima',
                    input: { fileRef: { rootId: 'workspace', relativePath: 'tx16w/set-b.ima' } },
                },
            ],
        });
    });
});

describe('sampleBankAssignmentRequest', () => {
    it('assigns selected Samples to one existing Sample Bank in request order', () => {
        expect(
            sampleBankAssignmentRequest('image-1', 5, {
                partitionIndex: 2,
                volumeName: 'Keys',
                sampleBankName: 'Existing Bank',
                sampleNames: ['Piano 2', 'Piano 10'],
            }),
        ).toEqual({
            imageId: 'image-1',
            expectedRevision: 5,
            manifest: {
                inline: {
                    schema_version: '1.0',
                    operations: [
                        {
                            id: 'sample-bank-assignment',
                            type: 'assign_sbac_members',
                            partition_index: 2,
                            volume_name: 'Keys',
                            sample_bank_name: 'Existing Bank',
                            sample_names: ['Piano 2', 'Piano 10'],
                        },
                    ],
                },
            },
            inputBindings: [],
        });
    });
});
