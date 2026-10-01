import { describe, expect, it } from 'vitest';
import { normalizeWaveformPreview, previewBinCount } from './waveformPreview';
import type { PreviewEnvelope } from './transport';

describe('waveform preview scale', () => {
    it.each([1, 2] as const)('normalizes %s-byte PCM without changing relative channel levels', (sampleWidthBytes) => {
        const scale = sampleWidthBytes === 1 ? 128 : 32768;
        const raw: PreviewEnvelope = {
            objectId: 'sample',
            lanes: [1, 0.5].map((level) => ({
                role: 'MONO',
                sourceObjectId: 'wave',
                sampleRate: 44100,
                sampleWidthBytes,
                storedFrameCount: 10,
                playbackStartFrame: 0,
                playbackLengthFrames: 10,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                bins: [{ minimum: -scale * level, maximum: (scale * level) / 2 }],
            })),
        };
        const result = normalizeWaveformPreview(raw);
        expect(result.lanes.map((lane) => lane.bins)).toEqual([
            [{ minimum: -1, maximum: 0.5 }],
            [{ minimum: -0.5, maximum: 0.25 }],
        ]);
        expect(raw.lanes[0]!.bins[0]!.minimum).toBe(-scale);
    });
    it('bounds resolution and groups nearby sizes into stable tiers', () => {
        expect(previewBinCount(300, 2)).toBe(1024);
        expect(previewBinCount(1200, 1.25)).toBe(2048);
        expect(previewBinCount(1201, 1.25)).toBe(2048);
        expect(previewBinCount(2000, 2)).toBe(4096);
        expect(previewBinCount(8000, 2)).toBe(4096);
    });
});
