import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { flushSync, tick } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import SampleTransportHarness from '../../../../test/SampleTransportHarness.svelte';
import type { AuditionState } from '../../../../lib/audio/auditionController';
import type { CachedAudition } from '../../../../lib/audio/auditionTypes';
import type { EditorAudioServices } from '../../../object-editor/audioContext';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import { prepareSampleDraft } from './audition';
import { sampleView } from './view.svelte';

vi.mock('./audition', async (original) => ({
    ...(await original<typeof import('./audition')>()),
    prepareSampleDraft: vi.fn(),
}));
vi.mock('../../../object-editor/audioSource', () => ({
    loadEditorAudio: vi.fn(async () => ({ sampleRate: 1000, lanes: [] })),
}));

beforeEach(() => vi.clearAllMocks());

function fixture(
    reverse = false,
    {
        readonly = false,
        conflict = '',
        inputError = '',
    }: { readonly?: boolean; conflict?: string; inputError?: string } = {},
) {
    const reason = readonly ? 'This image is read-only' : '';
    const document = {
        sessionId: 1,
        preferencesScope: {},
        detail: {
            object: { id: 'sample-1' },
            editing: { profile: 'a-series/sample', editable: !readonly, reason },
        },
        draft: new EditorDraft({
            'playback.start_frame': 100,
            'playback.length_frames': 200,
            root_key: 60,
            loop_mode: reverse ? 3 : 0,
            loop_start_frame: 0,
            loop_length_frames: 0,
        }),
        conflict,
        inputErrors: inputError ? { root_key: inputError } : {},
        get validation() {
            return document.conflict || Object.values(document.inputErrors).find(Boolean) || reason;
        },
        status: '',
        previewStatus: '',
    } as unknown as ObjectEditorDocument;
    let update = (_state: AuditionState) => {};
    const context = {
        sampleRate: 1000,
        destination: {},
        createGain: () => ({ gain: { value: 1 }, connect: vi.fn(), disconnect: vi.fn() }),
    } as unknown as AudioContext;
    vi.mocked(prepareSampleDraft).mockResolvedValue({
        objectId: 'sample-1',
        transient: true,
        descriptor: { frameCount: 200 },
    } as CachedAudition);
    const seekPrepared = vi.fn();
    let request: AbortController | undefined;
    const services: EditorAudioServices = {
        transport: {} as EditorAudioServices['transport'],
        audition: {
            state: { objectId: null, status: 'idle', playheadFrame: 0 },
            autoplay: false,
            playPrepared: vi.fn(async (_sessionId, objectId, prepare) => {
                request?.abort();
                const current = new AbortController();
                request = current;
                update({ objectId, status: 'preparing', playheadFrame: 0 });
                try {
                    await prepare(context, current.signal);
                } catch (error) {
                    if (current.signal.aborted) return;
                    throw error;
                }
                if (current.signal.aborted) return;
                update({
                    objectId,
                    status: 'playing',
                    playheadFrame: [3, 5].includes(Number(document.draft.values.loop_mode)) ? 199 : 0,
                    draft: true,
                });
            }),
            seekPrepared,
            stop: vi.fn(async () => {
                request?.abort();
                update({ objectId: null, status: 'idle', playheadFrame: 0 });
            }),
        },
    };
    const rendered = render(SampleTransportHarness, {
        document,
        services,
        onready: (callback) => (update = callback),
    });
    const view = sampleView(document);
    return {
        ...rendered,
        document,
        view,
        audition: services.audition,
        seekPrepared,
        update: (state: AuditionState) => flushSync(() => update(state)),
        play: async () => {
            await fireEvent.click(rendered.getByRole('button', { name: 'Play draft' }));
            await waitFor(() => expect(rendered.getByRole('button', { name: 'Stop draft preview' })).toBeTruthy());
        },
    };
}

describe('Sample transport cursor lifecycle', () => {
    it.each([false, true])('updates the idle origin when changing direction from reverse=%s', async (reverse) => {
        const result = fixture(reverse);
        expect(result.view.cursor).toBe(reverse ? 299 : 100);

        flushSync(() => result.document.draft.patch({ loop_mode: reverse ? 0 : 3 }));

        expect(result.view.cursor).toBe(reverse ? 100 : 299);
        await result.play();
        expect(result.seekPrepared).not.toHaveBeenCalled();
        expect(result.view.cursor).toBe(reverse ? 100 : 299);
    });

    it('auditions a clean read-only draft without clearing its editing validation', async () => {
        const result = fixture(false, { readonly: true });
        expect(result.document.validation).toBe('This image is read-only');

        await result.play();

        expect(prepareSampleDraft).toHaveBeenCalledOnce();
        expect(result.document.validation).toBe('This image is read-only');
        expect(result.document.draft.dirty).toBe(false);
    });

    it.each([{ conflict: 'The image changed externally' }, { inputError: 'Preview note must be a whole number' }])(
        'keeps read-only audition blocked by editable-state errors: %j',
        async (errors) => {
            const result = fixture(false, { readonly: true, ...errors });
            const play = result.getByRole('button', { name: 'Play draft' }) as HTMLButtonElement;
            expect(play.disabled).toBe(true);

            await fireEvent.click(play);

            expect(prepareSampleDraft).not.toHaveBeenCalled();
        },
    );

    it('still honors editing validation for a dirty read-only draft', async () => {
        const result = fixture(false, { readonly: true });
        flushSync(() => result.document.draft.patch({ root_key: 61 }));
        const play = result.getByRole('button', { name: 'Play draft' }) as HTMLButtonElement;
        expect(play.disabled).toBe(true);

        await fireEvent.click(play);

        expect(prepareSampleDraft).not.toHaveBeenCalled();
        expect(result.document.validation).toBe('This image is read-only');
    });

    it.each([false, true])('returns to the playback origin on Stop, reverse=%s', async (reverse) => {
        const result = fixture(reverse);
        await result.play();
        result.update({ objectId: 'sample-1', status: 'playing', playheadFrame: 75, draft: true });
        expect(result.view.cursor).toBe(175);

        await fireEvent.click(result.getByRole('button', { name: 'Stop draft preview' }));

        expect(result.view.cursor).toBe(reverse ? 299 : 100);
        result.seekPrepared.mockClear();
        await result.play();
        expect(result.seekPrepared).not.toHaveBeenCalled();
    });

    it.each([false, true])('returns to the playback origin on natural completion, reverse=%s', async (reverse) => {
        const result = fixture(reverse);
        await result.play();
        result.update({ objectId: 'sample-1', status: 'playing', playheadFrame: reverse ? 1 : 198, draft: true });

        result.update({ objectId: null, status: 'idle', playheadFrame: 0 });

        expect(result.view.cursor).toBe(reverse ? 299 : 100);
        result.seekPrepared.mockClear();
        await result.play();
        expect(result.seekPrepared).not.toHaveBeenCalled();
    });

    it('preserves a deliberate idle cursor selection for the next audition', async () => {
        const result = fixture();
        flushSync(() => (result.view.cursor = 175));
        flushSync(() => result.document.draft.patch({ level: 100 }));
        result.update({ objectId: null, status: 'idle', playheadFrame: 0 });
        expect(result.view.cursor).toBe(175);

        await result.play();

        await waitFor(() => expect(result.seekPrepared).toHaveBeenCalledWith(75));
    });

    it('does not seek a replacement when an older cancelled preparation finishes', async () => {
        const result = fixture();
        let finishCancelledRender!: () => void;
        vi.mocked(prepareSampleDraft).mockImplementationOnce(async (...args) => {
            await new Promise<void>((resolve) => (finishCancelledRender = resolve));
            args[7].throwIfAborted();
            throw new Error('Expected the previous preparation to be cancelled');
        });
        flushSync(() => (result.view.cursor = 175));
        await result.play();
        await waitFor(() => expect(finishCancelledRender).toBeTypeOf('function'));
        await fireEvent.click(result.getByRole('button', { name: 'Stop draft preview' }));
        await result.play();
        expect(result.view.cursor).toBe(100);
        result.seekPrepared.mockClear();

        finishCancelledRender();
        await vi.mocked(result.audition.playPrepared).mock.results[0]!.value;
        await tick();

        expect(result.seekPrepared).not.toHaveBeenCalled();
        expect(result.getByRole('button', { name: 'Stop draft preview' })).toBeTruthy();
    });
});
