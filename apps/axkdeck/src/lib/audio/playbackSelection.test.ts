import { describe, expect, it } from 'vitest';

import { inspectorSelectionStopsPlayback } from './playbackSelection';

describe('inspectorSelectionStopsPlayback', () => {
    it('stops active playback only when the inspector changes to another object', () => {
        expect(inspectorSelectionStopsPlayback(null, 'SMPL-B')).toBe(false);
        expect(inspectorSelectionStopsPlayback('SMPL-A', 'SMPL-A')).toBe(false);
        expect(inspectorSelectionStopsPlayback('SMPL-A', 'SMPL-B')).toBe(true);
        expect(inspectorSelectionStopsPlayback('SBNK-A', 'PROG-B')).toBe(true);
    });
});
