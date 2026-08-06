import { describe, expect, it } from 'vitest';
import { exportSelectionLabel, imageSessionExportRoots } from './selection';
import type { PackageExportSelection } from '../../lib/types';

describe('export selection', () => {
    it('translates volume and object roots without losing scope identity', () => {
        const items: PackageExportSelection[] = [
            {
                kind: 'VOLUME',
                contentId: 'volume-brass',
                name: 'Brass',
                partitionIndex: 3,
                volumeName: 'Brass',
                typeLabel: 'Volume',
            },
            {
                kind: 'SBNK',
                objectId: 'sample-7',
                name: 'Snare',
                partitionIndex: 3,
                partitionName: 'Partition 3',
                volumeName: 'Brass',
                typeLabel: 'Sample',
            },
        ];

        expect(imageSessionExportRoots(items)).toEqual([
            { kind: 'VOLUME', contentId: 'volume-brass' },
            { kind: 'SBNK', objectId: 'sample-7' },
        ]);
        expect(exportSelectionLabel(items)).toBe('2 objects');
    });
});
