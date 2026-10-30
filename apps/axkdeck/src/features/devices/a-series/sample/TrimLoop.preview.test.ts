import { act, fireEvent, render, waitFor } from '@testing-library/svelte';
import { fromStore, writable } from 'svelte/store';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ObjectDetail, PreviewEnvelope } from '../../../../lib/transport';
import type { SampleWaveformPreview } from '../../../../lib/types';
import DeviceEditorHostHarness from '../../../../test/DeviceEditorHostHarness.svelte';
import { sampleConversionFixture, sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import type { EditorAudioServices } from '../../../object-editor/audioContext';
import { ObjectEditorWorkflow } from '../../../object-editor/workflow.svelte';
import { sampleView } from './view.svelte';

let width = 1200;
const observers = new Set<(entries: ResizeObserverEntry[]) => void>();

beforeEach(() => {
    width = 1200;
    observers.clear();
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(() => new DOMRect(0, 0, width, 100));
    vi.stubGlobal(
        'ResizeObserver',
        class {
            constructor(private readonly callback: (entries: ResizeObserverEntry[]) => void) {}
            observe() {
                observers.add(this.callback);
            }
            unobserve() {}
            disconnect() {
                observers.delete(this.callback);
            }
        },
    );
});
afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

function envelope(count: number): PreviewEnvelope {
    return {
        imageId: 'image-1',
        documentId: 'document-1',
        revision: 1,
        warnings: [],
        objectId: 'A',
        lanes: ['LEFT', 'RIGHT'].map((role) => ({
            role,
            sourceObjectId: `wave-${role}`,
            sampleRate: 44100,
            sampleWidthBytes: 2,
            nativeBitDepth: 16,
            sampleRateApproximate: false,
            storedFrameCount: 16384,
            playbackStartFrame: 0,
            playbackLengthFrames: 16384,
            loopStartFrame: 0,
            loopLengthFrames: 16384,
            bins: Array.from({ length: count }, () => ({ minimum: -0.25, maximum: 0.25 })),
        })),
    } as PreviewEnvelope;
}

function setup() {
    const detail = {
        formatId: 'yamaha.a-series',
        profile: 'a-series',
        image: { revision: 1 },
        object: { id: 'A', key: 'A', name: 'Sample A', type: 'SBNK' },
        formatConversion: sampleConversionFixture(),
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 16384 },
            playbackWindow: { start_frame: 0, length_frames: 16384 },
            maximumFrames: 16384,
            canEditPlayback: true,
            eqCoefficients: [],
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
    const requests: { resolve: (value: PreviewEnvelope) => void; reject: (reason: Error) => void }[] = [];
    const preview = vi.fn(() => new Promise<PreviewEnvelope>((resolve, reject) => requests.push({ resolve, reject })));
    const transport = {
        objectDetail: vi.fn(async () => detail),
        startObjectParameterEdit: vi.fn(),
        waitForJob: vi.fn(),
    };
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: async () => {},
        stopPlayback: () => {},
        status: () => {},
    });
    const autoplay = fromStore(writable(false));
    const audio: EditorAudioServices = {
        transport: {
            preview,
            audioIdentity: () => ({ imageId: 'image-1', documentId: 'document-1', revision: 1 }),
        } as unknown as EditorAudioServices['transport'],
        audition: {
            state: { objectId: null, status: 'idle', playheadFrame: 0 },
            get autoplay() {
                return autoplay.current;
            },
            set autoplay(value) {
                autoplay.current = value;
            },
            playPrepared: vi.fn(),
            seekPrepared: vi.fn(),
            stop: vi.fn(),
        },
    };
    const view = render(DeviceEditorHostHarness, {
        workflow,
        audio,
        preview: { preview: envelope(1024), previewState: 'ready' } as SampleWaveformPreview,
    });
    return { view, preview, requests, transport, workflow, detail };
}

async function resize(next: number) {
    await act(() => {
        width = next;
        for (const observer of [...observers]) observer([]);
    });
    await new Promise((resolve) => setTimeout(resolve, 130));
}

describe('TrimLoop adaptive preview requests', () => {
    it('keeps snap off until audio loading succeeds and disables the switch while pending', async () => {
        const fixture = setup();
        const control = await fixture.view.findByRole('switch', { name: 'Zero-cross snap' });
        const model = sampleView(fixture.workflow.find(1, 'A')!);
        let complete!: (value: NonNullable<typeof model.source> | undefined) => void;
        const load = vi.spyOn(model, 'load').mockImplementation(async () => {
            model.loading = true;
            const source = await new Promise<NonNullable<typeof model.source> | undefined>(
                (resolve) => (complete = resolve),
            );
            model.loading = false;
            return source;
        });
        await fireEvent.click(control);
        expect(control.getAttribute('aria-checked')).toBe('false');
        expect((control as HTMLButtonElement).disabled).toBe(true);
        await act(async () => complete({} as NonNullable<typeof model.source>));
        expect(control.getAttribute('aria-checked')).toBe('true');
        expect((control as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(control);
        expect(control.getAttribute('aria-checked')).toBe('false');
        expect(load).toHaveBeenCalledOnce();
    });

    it('does not enable snap when source audio loading fails', async () => {
        const fixture = setup();
        const control = await fixture.view.findByRole('switch', { name: 'Zero-cross snap' });
        const model = sampleView(fixture.workflow.find(1, 'A')!);
        vi.spyOn(model, 'load').mockResolvedValue(undefined);
        await fireEvent.click(control);
        expect(model.snap).toBe(false);
        expect(control.getAttribute('aria-checked')).toBe('false');
    });

    it('bounds unresolved requests and coalesces repeated tier changes to the latest desired resolution', async () => {
        const fixture = setup();
        await waitFor(() =>
            expect(fixture.preview).toHaveBeenCalledExactlyOnceWith(1, 'A', 2048, {
                readIntent: 'REFINEMENT',
                signal: expect.any(AbortSignal),
            }),
        );

        for (const size of [2200, 1100, 2300]) await resize(size);

        expect(fixture.preview).toHaveBeenCalledOnce();
        expect(fixture.view.getByText('LEFT')).toBeTruthy();
        fixture.requests[0]!.resolve(envelope(2048));
        await waitFor(() => expect(fixture.preview).toHaveBeenCalledTimes(2));
        expect(fixture.preview).toHaveBeenLastCalledWith(1, 'A', 4096, {
            readIntent: 'REFINEMENT',
            signal: expect.any(AbortSignal),
        });
        fixture.requests[1]!.resolve(envelope(4096));
        await resize(1000);
        await resize(2300);
        expect(fixture.preview).toHaveBeenCalledTimes(2);
    });

    it('preserves a usable preview and does not retry a failed tier after width changes', async () => {
        const fixture = setup();
        await waitFor(() => expect(fixture.preview).toHaveBeenCalledOnce());
        fixture.requests[0]!.reject(new Error('Adaptive preview unavailable'));
        await fixture.view.findByText('Adaptive preview unavailable');
        await resize(900);
        await resize(1200);

        expect(fixture.preview).toHaveBeenCalledOnce();
        expect(fixture.view.getByText('LEFT')).toBeTruthy();
        expect(fixture.view.getByText('RIGHT')).toBeTruthy();
    });

    it('keeps a sharper audition preview when a coarser direct request completes late', async () => {
        const fixture = setup();
        await waitFor(() =>
            expect(fixture.preview).toHaveBeenCalledExactlyOnceWith(1, 'A', 2048, {
                readIntent: 'REFINEMENT',
                signal: expect.any(AbortSignal),
            }),
        );
        await fixture.view.rerender({
            preview: { preview: envelope(4096), previewState: 'ready' } as SampleWaveformPreview,
        });
        await resize(2300);
        fixture.requests[0]!.resolve(envelope(2048));
        await resize(2300);

        expect(fixture.preview).toHaveBeenCalledOnce();
        expect(fixture.view.getByText('LEFT')).toBeTruthy();
        expect(fixture.view.getByText('RIGHT')).toBeTruthy();
    });

    it('ignores a completed preview after the current detail is replaced', async () => {
        const fixture = setup();
        await waitFor(() => expect(fixture.preview).toHaveBeenCalledOnce());
        const document = fixture.workflow.find(1, 'A')!;
        fixture.transport.objectDetail.mockResolvedValueOnce({ ...fixture.detail });
        await act(async () => {
            await fixture.workflow.check(document);
        });
        fixture.requests[0]!.resolve(envelope(2048));
        await waitFor(() => expect(fixture.preview).toHaveBeenCalledTimes(2));
        expect(fixture.preview).toHaveBeenLastCalledWith(1, 'A', 2048, {
            readIntent: 'REFINEMENT',
            signal: expect.any(AbortSignal),
        });
        fixture.requests[1]!.resolve(envelope(2048));
        await resize(1200);
        expect(fixture.preview).toHaveBeenCalledTimes(2);
    });

    it('does not publish a completed request after the editor unmounts', async () => {
        const fixture = setup();
        await waitFor(() => expect(fixture.preview).toHaveBeenCalledOnce());
        await fixture.view.rerender({ visible: false });
        fixture.requests[0]!.resolve(envelope(2048));
        await resize(2300);

        expect(fixture.preview).toHaveBeenCalledOnce();
        expect(fixture.view.queryByRole('region', { name: 'Sample editor' })).toBeNull();
    });
});
