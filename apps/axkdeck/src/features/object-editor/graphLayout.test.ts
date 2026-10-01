import { describe, expect, it } from 'vitest';
import { graphSplitRatio } from './graphLayout.svelte';

describe('graph column sizing', () => {
    it('keeps both columns usable at either drag limit', () => {
        expect(graphSplitRatio(0.5, 1000)).toBe(0.5);
        expect(graphSplitRatio(0.1, 1000)).toBe(0.36);
        expect(graphSplitRatio(0.9, 1000)).toBe(0.64);
        expect(graphSplitRatio(0.9, 600)).toBe(0.5);
    });
});
