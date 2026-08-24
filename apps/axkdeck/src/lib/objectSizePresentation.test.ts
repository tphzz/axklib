import { describe, expect, it } from 'vitest';
import { objectSizeSummary, objectSizeTooltip } from './objectSizePresentation';

describe('object size presentation', () => {
    it('distinguishes own bytes from an exact dependency closure', () => {
        const object = { storedSizeBytes: 1024, sizeWithDependenciesBytes: 4096 };

        expect(objectSizeSummary(object)).toBe('1 KiB · 4 KiB incl. deps.');
        expect(objectSizeTooltip(object)).toBe('Object size: 1 KiB\nObject size with deps.: 4 KiB');
    });

    it('does not present an incomplete dependency graph as a lower bound', () => {
        const object = { storedSizeBytes: 128, sizeWithDependenciesBytes: null };

        expect(objectSizeSummary(object)).toBe('128 B · deps. unavailable');
        expect(objectSizeTooltip(object)).toBe('Object size: 128 B\nObject size with deps.: Unavailable');
    });
});
