import { describe, expect, it } from 'vitest';
import { importCapacityGroups } from './importCapacity';
import { capacityConflict } from '../test/importCapacityFixture';

describe('import capacity presentation', () => {
    it('groups by partition rather than object, package, volume or diagnostic text', () => {
        const conflicts = Array.from({ length: 63 }, (_, index) => ({
            ...capacityConflict(0, `wave-${index}`),
            packageIndex: index % 2,
            volumeName: `Volume ${index}`,
        }));
        conflicts.push({ ...capacityConflict(1), packageIndex: 0 });
        const groups = importCapacityGroups(conflicts, [{ partitionIndex: 0, name: 'Drums' }]);
        expect(groups.map((group) => [group.name, group.conflicts.length])).toEqual([
            ['Drums', 63],
            ['Partition 2', 1],
        ]);
    });
    it('does not hide unknown, record-capacity or naming errors based on their messages', () => {
        const conflicts = ['SFS_NAME_CONFLICT', 'SFS_RECORD_CAPACITY_EXHAUSTED', 'UNKNOWN'].map((code) => ({
            ...capacityConflict(0),
            code,
            message: 'partition has 0 free cluster(s)',
        }));
        expect(importCapacityGroups(conflicts, [])).toEqual([]);
        expect(importCapacityGroups([capacityConflict(null)], [])[0].name).toBe('the destination partition');
    });
});
