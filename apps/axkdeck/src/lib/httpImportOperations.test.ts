import { describe, expect, it } from 'vitest';

import { clientUploadLocation, serverFileLocation } from './storageLocations';
import {
    sampleBankAssignmentRequest,
    sampleBankCreationRequest,
    tx16wDiskSetImportRequest,
} from './httpImportOperations';

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
