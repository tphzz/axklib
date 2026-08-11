import { describe, expect, it } from 'vitest';
import type { SamplerObject } from './transport';
import { objectPresentationName } from './objectPresentation';

function object(objectType: string, name: string): SamplerObject {
    return {
        key: `${objectType}-${name}`,
        objectType,
        name,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: objectType,
        sfsId: 0,
        storedSizeBytes: 128,
        sampleRate: 0,
        rootKey: 0,
        frameCount: 0,
        sampleWidthBytes: 0,
    };
}

describe('objectPresentationName', () => {
    it('uses the canonical SBAC name instead of its sampler-menu decoration', () => {
        const bank = object('SBAC', 'Indian');
        expect(objectPresentationName(bank, new Map([[bank.key, 'B Indian']]))).toBe('Indian');
    });

    it('does not strip a legitimate leading B from the canonical SBAC name', () => {
        const bank = object('SBAC', 'Basses');
        expect(objectPresentationName(bank, new Map([[bank.key, 'B Basses']]))).toBe('Basses');
    });

    it('retains semantic display names for other object types', () => {
        const sample = object('SBNK', 'Raw name');
        expect(objectPresentationName(sample, new Map([[sample.key, 'Visible name']]))).toBe('Visible name');
    });
});
