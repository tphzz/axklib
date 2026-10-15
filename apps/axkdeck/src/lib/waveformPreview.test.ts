import { describe, expect, it } from 'vitest';
import { normalizeWaveformPreview, previewBinCount } from './waveformPreview';
import type { PreviewEnvelope } from './transport';

describe('waveform preview scale', () => {
    it.each([1, 2] as const)('normalizes %s-byte PCM without changing relative channel levels', (sampleWidthBytes) => {
        const scale = sampleWidthBytes === 1 ? 128 : 32768;
        const raw: PreviewEnvelope = {
            imageId: 'image',
            documentId: 'document',
            revision: 3,
            objectId: 'sample',
            warnings: [],
            lanes: [1, 0.5].map((level) => ({
                role: 'MONO',
                sourceObjectId: 'wave',
                sampleRate: 44100,
                sampleWidthBytes,
                nativeBitDepth: sampleWidthBytes * 8,
                sampleRateApproximate: false,
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
    it('normalizes expanded 12-bit PCM using its transport width and retains source identity and warnings', () => {
        const raw: PreviewEnvelope = {
            imageId: 'tx-image',
            documentId: 'tx-document',
            revision: 4,
            objectId: 'tx-wave',
            warnings: ['Sample rate is uncertain'],
            lanes: [
                {
                    role: 'MONO',
                    sourceObjectId: 'tx-wave',
                    sampleRate: 33333,
                    sampleWidthBytes: 2,
                    nativeBitDepth: 12,
                    sampleRateApproximate: true,
                    storedFrameCount: 5,
                    playbackStartFrame: 0,
                    playbackLengthFrames: 5,
                    loopStartFrame: 2,
                    loopLengthFrames: 3,
                    bins: [{ minimum: -32768, maximum: 32752 }],
                },
            ],
        };
        const result = normalizeWaveformPreview(raw);
        expect(result).toMatchObject({
            imageId: 'tx-image',
            documentId: 'tx-document',
            revision: 4,
            objectId: 'tx-wave',
            warnings: raw.warnings,
        });
        expect(result.lanes[0]).toMatchObject({
            nativeBitDepth: 12,
            sampleRateApproximate: true,
            loopStartFrame: 2,
            loopLengthFrames: 3,
            bins: [{ minimum: -1, maximum: 32752 / 32768 }],
        });
        expect(raw.lanes[0]!.bins[0]!.minimum).toBe(-32768);
    });
    it('bounds resolution and groups nearby sizes into stable tiers', () => {
        expect(previewBinCount(300, 2)).toBe(1024);
        expect(previewBinCount(1200, 1.25)).toBe(2048);
        expect(previewBinCount(1201, 1.25)).toBe(2048);
        expect(previewBinCount(2000, 2)).toBe(4096);
        expect(previewBinCount(8000, 2)).toBe(4096);
    });
});
