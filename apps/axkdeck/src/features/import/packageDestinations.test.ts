import { describe, expect, it } from 'vitest';
import { importDestination, initialImportDestination } from './packageDestinations';

describe('importDestination', () => {
    it('leaves the chooser unresolved without a partition or volume selection', () => {
        expect(initialImportDestination(null)).toBeNull();
    });

    it('preserves the exact name of an existing sampler volume', () => {
        expect(importDestination('existing', 2, '  Existing Volume  ')).toEqual({
            kind: 'EXISTING_VOLUME',
            partitionIndex: 2,
            volumeName: '  Existing Volume  ',
        });
    });

    it('accepts a valid new sampler volume name without rewriting it', () => {
        expect(importDestination('create', 0, 'New Volume')).toEqual({
            kind: 'CREATE_VOLUME',
            partitionIndex: 0,
            volumeName: 'New Volume',
        });
    });

    it.each([' New Volume', 'New Volume ', '12345678901234567', 'New\nVolume'])(
        'rejects the invalid new sampler volume name %j',
        (volumeName) => {
            expect(importDestination('create', 0, volumeName)).toBeNull();
        },
    );
});
