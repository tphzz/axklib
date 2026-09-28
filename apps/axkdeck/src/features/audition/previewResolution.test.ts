import { describe, expect, it, vi } from 'vitest';
import { AuditionWorkflow } from './workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { ImageTransport, PreviewEnvelope, SamplerObject } from '../../lib/transport';
import type { WaveDataItem } from '../../lib/types';

function fixture(initialBins = 0) {
    const bins = (count: number) => Array.from({ length: count }, () => ({ minimum: -0.25, maximum: 0.25 }));
    const item: WaveDataItem = {
        id: 'wave',
        name: 'Wave',
        note: 'C3',
        duration: '0.18 s',
        sampleRate: '44.1 kHz',
        bitDepth: '16-bit',
        channels: 'Mono',
        storedSizeBytes: 16000,
        objectKey: 'wave',
        object: { storedFrameCount: 8000 } as SamplerObject,
        waveform: bins(initialBins),
        previewState: initialBins ? 'ready' : 'idle',
    };
    const requests: { resolve: (value: PreviewEnvelope) => void; reject: (reason: Error) => void }[] = [];
    const preview = vi.fn(() => new Promise<PreviewEnvelope>((resolve, reject) => requests.push({ resolve, reject })));
    const catalog = { waveData: [item], samplePreviewStates: {} } as unknown as CatalogWorkflow;
    const status = vi.fn();
    const workflow = new AuditionWorkflow({
        transport: { preview } as unknown as ImageTransport,
        catalog,
        sessionId: () => 1,
        workspaceView: () => 'wave-data',
        selection: () => ({ items: [], anchors: {} }),
        setSelection: () => undefined,
        setWorkspaceView: () => undefined,
        setInspectorOpen: () => undefined,
        setStatus: status,
        requestCompanionDisks: () => undefined,
    });
    const result = (count: number): PreviewEnvelope => ({
        objectId: 'wave',
        lanes: [
            {
                role: 'MONO',
                sourceObjectId: 'wave',
                sampleRate: 44100,
                sampleWidthBytes: 2,
                storedFrameCount: 8000,
                playbackStartFrame: 0,
                playbackLengthFrames: 8000,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                bins: bins(count),
            },
        ],
    });
    return { workflow, catalog, requests, preview, result, status };
}

describe('adaptive waveform previews', () => {
    it('retains the visible preview while upgrading and deduplicates the requested resolution', async () => {
        const f = fixture(1024);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 2048);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 2048);
        expect(f.preview).toHaveBeenCalledExactlyOnceWith(1, 'wave', 2048);
        expect(f.catalog.waveData[0]!.previewState).toBe('ready');
        expect(f.catalog.waveData[0]!.waveform).toHaveLength(1024);
        f.requests[0]!.resolve(f.result(2048));
        await vi.waitFor(() => expect(f.catalog.waveData[0]!.waveform).toHaveLength(2048));
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 1024);
        expect(f.preview).toHaveBeenCalledOnce();
    });
    it('does not replace a sharper preview with a late coarse response', async () => {
        const f = fixture();
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 1024);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 4096);
        f.requests[1]!.resolve(f.result(4096));
        await vi.waitFor(() => expect(f.catalog.waveData[0]!.waveform).toHaveLength(4096));
        f.requests[0]!.resolve(f.result(1024));
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(f.catalog.waveData[0]!.waveform).toHaveLength(4096);
    });
    it('keeps a usable preview after an upgrade fails', async () => {
        const f = fixture(1024);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 4096);
        f.requests[0]!.reject(new Error('preview unavailable'));
        await vi.waitFor(() => expect(f.status).toHaveBeenCalled());
        expect(f.catalog.waveData[0]!.previewState).toBe('ready');
        expect(f.catalog.waveData[0]!.waveform).toHaveLength(1024);
    });
    it('discards responses from an invalidated selection generation', async () => {
        const f = fixture(1024);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 4096);
        f.workflow.resetPreviewQueue();
        f.requests[0]!.resolve(f.result(4096));
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(f.catalog.waveData[0]!.waveform).toHaveLength(1024);
    });
});
