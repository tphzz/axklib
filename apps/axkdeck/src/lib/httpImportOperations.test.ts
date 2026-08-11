import { describe, expect, it } from 'vitest';

import { sampleBankAssignmentRequest, sampleBankCreationRequest } from './httpImportOperations';

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
