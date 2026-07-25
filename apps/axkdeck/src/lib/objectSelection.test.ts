import { describe, expect, it } from 'vitest';
import { updateObjectSelection } from './objectSelection';

const domain = ['a', 'b', 'c', 'd', 'e'];

describe('object selection', () => {
    it('supports replacement, toggling, ranges, additive ranges, and filtered select-all', () => {
        expect(updateObjectSelection([], '', domain, domain, 'b', 'replace')).toEqual({
            objectIds: ['b'],
            anchorId: 'b',
        });
        expect(updateObjectSelection(['b'], 'b', domain, domain, 'd', 'range')).toEqual({
            objectIds: ['b', 'c', 'd'],
            anchorId: 'b',
        });
        expect(updateObjectSelection(['b', 'c', 'd'], 'b', domain, domain, 'c', 'toggle').objectIds).toEqual([
            'b',
            'd',
        ]);
        expect(updateObjectSelection(['a'], 'a', domain, domain, 'c', 'add-range').objectIds).toEqual(['a', 'b', 'c']);
        expect(updateObjectSelection([], '', domain, ['b', 'd'], 'b', 'all').objectIds).toEqual(['b', 'd']);
    });

    it('falls back to the target when the range anchor is filtered out', () => {
        expect(updateObjectSelection(['a'], 'a', domain, ['c', 'd'], 'd', 'range')).toEqual({
            objectIds: ['d'],
            anchorId: 'd',
        });
    });
});
