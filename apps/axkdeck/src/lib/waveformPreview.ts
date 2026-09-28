import type { PreviewEnvelope } from './transport';

// The wire envelope is signed PCM; every display uses full-scale floating point.
export function normalizeWaveformPreview(preview: PreviewEnvelope): PreviewEnvelope {
    return {
        ...preview,
        lanes: preview.lanes.map((lane) => {
            if (lane.sampleWidthBytes !== 1 && lane.sampleWidthBytes !== 2)
                throw new Error('Waveform preview has an unsupported sample width');
            const scale = lane.sampleWidthBytes === 1 ? 128 : 32768;
            return {
                ...lane,
                bins: lane.bins.map(({ minimum, maximum }) => ({ minimum: minimum / scale, maximum: maximum / scale })),
            };
        }),
    };
}

export function previewBinCount(width: number, ratio: number): number {
    return Math.min(4096, Math.max(1024, 2 ** Math.ceil(Math.log2(Math.max(1, width * ratio)))));
}
