import { describe, expect, it, vi } from 'vitest';
import { AuditionWorkflow } from './workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { AudioIdentity, ImageTransport, PreviewEnvelope, SamplerObject } from '../../lib/transport';
import type { WaveDataItem } from '../../lib/types';
import { AuditionController } from '../../lib/audio/auditionController';

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
    let identity: AudioIdentity = { imageId: 'image-1', documentId: 'document-1', revision: 1 };
    const requests: {
        resolve: (value: PreviewEnvelope) => void;
        reject: (reason: Error) => void;
        signal?: AbortSignal;
    }[] = [];
    const preview = vi.fn(
        (_session: number, _objectId: string, _bins: number, { signal }: Parameters<ImageTransport['preview']>[3]) =>
            new Promise<PreviewEnvelope>((resolve, reject) => requests.push({ resolve, reject, signal })),
    );
    const catalog = {
        waveData: [item],
        samplePreviewStates: {},
        relationships: [],
        sampleBanks: [],
        samples: [],
        editorObjectIds: {},
        inspectorObjectId: '',
    } as unknown as CatalogWorkflow;
    const status = vi.fn();
    const workflow = new AuditionWorkflow({
        transport: { preview, audioIdentity: () => identity } as unknown as ImageTransport,
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
        ...identity,
        objectId: 'wave',
        warnings: [],
        lanes: [
            {
                role: 'MONO',
                sourceObjectId: 'wave',
                sampleRate: 44100,
                sampleWidthBytes: 2,
                nativeBitDepth: 16,
                sampleRateApproximate: false,
                storedFrameCount: 8000,
                playbackStartFrame: 0,
                playbackLengthFrames: 8000,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                bins: bins(count),
            },
        ],
    });
    return {
        workflow,
        catalog,
        requests,
        preview,
        result,
        status,
        changeIdentity: (next: Partial<AudioIdentity>) => {
            identity = { ...identity, ...next };
        },
    };
}

describe('adaptive waveform previews', () => {
    it('retains an unchanged in-flight selection and defers audio until its actual draw without diagnostics', async () => {
        const prefetch = vi.spyOn(AuditionController.prototype, 'prefetch').mockResolvedValue(undefined);
        const f = fixture();
        try {
            await f.workflow.selectWaveData(f.catalog.waveData[0]!);
            await f.workflow.selectWaveData(f.catalog.waveData[0]!);
            expect(f.preview).toHaveBeenCalledOnce();
            expect(f.requests[0]!.signal?.aborted).toBe(false);
            f.workflow.prefetchObject('wave');
            expect(prefetch).not.toHaveBeenCalled();
            f.requests[0]!.resolve(f.result(1024));
            await vi.waitFor(() => expect(f.catalog.waveData[0]!.previewState).toBe('ready'));
            expect(prefetch).not.toHaveBeenCalled();
            f.workflow.waveformDrawn('wave', { bins: 1024, width: 200, height: 24 });
            expect(prefetch).toHaveBeenCalledExactlyOnceWith(1, 'wave');
            await f.workflow.selectWaveData(f.catalog.waveData[0]!);
            f.workflow.prefetchObject('wave');
            expect(prefetch).toHaveBeenCalledTimes(1);
            f.workflow.waveformDrawn('wave', { bins: 1024, width: 200, height: 24 });
            expect(prefetch).toHaveBeenCalledTimes(1);
        } finally {
            await f.workflow.dispose();
            prefetch.mockRestore();
        }
    });
    it('retains the visible preview while upgrading and deduplicates the requested resolution', async () => {
        const f = fixture(1024);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 2048);
        f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 2048);
        expect(f.preview).toHaveBeenCalledExactlyOnceWith(1, 'wave', 2048, {
            signal: expect.any(AbortSignal),
            readIntent: 'REFINEMENT',
        });
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
        expect(f.requests[0]!.signal?.aborted).toBe(true);
        f.requests[0]!.resolve(f.result(4096));
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(f.catalog.waveData[0]!.waveform).toHaveLength(1024);
    });
    it.each([{ revision: 2 }, { documentId: 'document-2' }])(
        'discards an upgrade after its audio identity changes: %j',
        async (change) => {
            const f = fixture(1024);
            f.workflow.requestWaveformPreview(f.catalog.waveData[0]!, 4096);
            const oldReply = f.result(4096);
            f.changeIdentity(change);
            f.requests[0]!.resolve(oldReply);
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(f.catalog.waveData[0]!.waveform).toHaveLength(1024);
            expect(f.status).not.toHaveBeenCalled();
        },
    );
});
