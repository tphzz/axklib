import { describe, expect, it } from 'vitest';
import { packageImportDestination } from './packageDestinations';

describe('packageImportDestination', () => {
    it('preserves the exact name of an existing sampler volume', () => {
        expect(packageImportDestination('existing', 2, '  Existing Volume  ')).toEqual({
            kind: 'EXISTING_VOLUME',
            partitionIndex: 2,
            volumeName: '  Existing Volume  ',
        });
    });

    it('accepts a valid new sampler volume name without rewriting it', () => {
        expect(packageImportDestination('create', 0, 'New Volume')).toEqual({
            kind: 'CREATE_VOLUME',
            partitionIndex: 0,
            volumeName: 'New Volume',
        });
    });

    it.each([' New Volume', 'New Volume ', '12345678901234567', 'New\nVolume'])(
        'rejects the invalid new sampler volume name %j',
        (volumeName) => {
            expect(packageImportDestination('create', 0, volumeName)).toBeNull();
        },
    );
});
