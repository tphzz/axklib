import { describe, expect, it } from 'vitest';
import { compareNaturalNames, compareNamedItems } from './naturalSort';

describe('natural name ordering', () => {
    it('orders embedded numeric runs by value instead of text', () => {
        const names = ['LoopDiv13', 'LoopDiv02', 'LoopDiv1', 'LoopDiv00', 'LoopDiv10'];

        expect(names.toSorted(compareNaturalNames)).toEqual([
            'LoopDiv00',
            'LoopDiv1',
            'LoopDiv02',
            'LoopDiv10',
            'LoopDiv13',
        ]);
    });

    it('uses exact spelling and object identity to make equivalent visible names deterministic', () => {
        const items = [
            { id: 'object-2', name: 'slice2' },
            { id: 'object-3', name: 'Slice02' },
            { id: 'object-1', name: 'Slice2' },
        ];

        expect(items.toSorted(compareNamedItems)).toEqual([
            { id: 'object-3', name: 'Slice02' },
            { id: 'object-1', name: 'Slice2' },
            { id: 'object-2', name: 'slice2' },
        ]);
    });
});
